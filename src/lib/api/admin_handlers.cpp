#include "api/admin_handlers.hpp"

#include "logging/business_log_reader.hpp"

#include <array>
#include <chrono>
#include <charconv>
#include <optional>
#include <string>
#include <utility>

namespace confide::api {

namespace {

using common::ErrCode;
using common::Error;

constexpr std::string_view kTag = "admin";

std::string_view sv(beast::string_view s) {
  return {s.data(), s.size()};
}

Result<common::TransactionId> id_param(const RequestContext& ctx) {
  const auto* text = ctx.param("id");
  if (text == nullptr) {
    return std::unexpected(Error::make(ErrCode::internal, "route has no {id}"));
  }
  return common::TransactionId::parse(*text);
}

// Query value `key` as ISO-8601; nullopt if absent.
Result<std::optional<common::Timestamp>> time_query(const RequestContext& ctx,
                                                    std::string_view key) {
  const auto text = ctx.query_value(key);
  if (!text) {
    return std::nullopt;
  }
  auto ts = common::parse_iso8601(*text);
  if (!ts) {
    return std::unexpected(
        Error::make(ErrCode::validation, std::string{key} + ": expected YYYY-MM-DDTHH:MM:SSZ"));
  }
  return *ts;
}

Result<transaction::AdminFilter> filter_from(const RequestContext& ctx) {
  transaction::AdminFilter filter;
  if (const auto user = ctx.query_value("user")) {
    filter.user = std::string{*user};
  }
  if (const auto state = ctx.query_value("state")) {
    CFD_TRY(parsed, transaction::parse_state(*state));
    filter.state = parsed;
  }
  CFD_TRY(from, time_query(ctx, "from"));
  CFD_TRY(to, time_query(ctx, "to"));
  filter.from = from;
  filter.to = to;
  return filter;
}

}  // namespace

AdminHandlers::AdminHandlers(transaction::TransactionService& service, const IAuthenticator& auth,
                             const IAdminSessions& sessions, logging::BusinessLog& blog,
                             logging::TechLog& tlog, const common::IClock& clock,
                             AdminOptions options)
    : service_(service)
    , auth_(auth)
    , sessions_(sessions)
    , blog_(blog)
    , tlog_(tlog)
    , clock_(clock)
    , options_(std::move(options))
    , started_at_(clock.now()) {}

Result<void> AdminHandlers::register_routes(Router& router) {
  using http::verb;
  using M = asio::awaitable<void> (AdminHandlers::*)(RequestContext&);
  struct Def {
    verb method;
    const char* pattern;
    Access access;
    M fn;
  };
  const std::array defs{
      Def{verb::post, "/admin/token", Access::admin_password, &AdminHandlers::login},
      Def{verb::delete_, "/admin/token", Access::admin, &AdminHandlers::logout},
      Def{verb::get, "/admin/transactions", Access::admin, &AdminHandlers::list},
      Def{verb::get, "/admin/transactions/{id}", Access::admin, &AdminHandlers::get},
      Def{verb::delete_, "/admin/transactions/{id}", Access::admin, &AdminHandlers::remove},
      Def{verb::put, "/admin/transactions/{id}/retention", Access::admin,
          &AdminHandlers::retention},
      Def{verb::get, "/admin/status", Access::admin, &AdminHandlers::status},
      Def{verb::get, "/admin/sessions", Access::admin, &AdminHandlers::sessions},
      Def{verb::delete_, "/admin/sessions/{user}", Access::admin, &AdminHandlers::revoke_user},
      Def{verb::get, "/admin/log", Access::admin, &AdminHandlers::log},
  };
  for (const auto& d : defs) {
    CFD_TRYV(router.add(Route{
        .method = d.method,
        .pattern = d.pattern,
        .access = d.access,
        .handler = [this, fn = d.fn](RequestContext& ctx) { return (this->*fn)(ctx); },
    }));
  }
  return {};
}

asio::awaitable<void> AdminHandlers::login(RequestContext& ctx) {
  auto issued = sessions_.issue_admin_token(ctx.caller.name);
  if (!issued) {
    co_return co_await fail(ctx, issued.error());
  }
  tlog_.info(kTag, "admin token issued for {}", ctx.caller.name);
  auto res = Response::json(http::status::ok,
                            encode_token(issued->token, ctx.caller.name, issued->expires_at));
  res.headers.emplace_back("Cache-Control", "no-store");
  co_await ctx.exchange.send(std::move(res));
}

asio::awaitable<void> AdminHandlers::logout(RequestContext& ctx) {
  auth_.revoke_token(ctx.exchange.request());
  tlog_.info(kTag, "admin token revoked for {}", ctx.caller.name);
  co_await ctx.exchange.send(Response::no_content());
}

asio::awaitable<void> AdminHandlers::list(RequestContext& ctx) {
  auto filter = filter_from(ctx);
  if (!filter) {
    co_return co_await fail(ctx, filter.error());
  }
  const auto txs = service_.admin_list(ctx.caller, *filter);
  co_await ctx.exchange.send(Response::json(http::status::ok, encode_admin_transaction_list(txs)));
}

asio::awaitable<void> AdminHandlers::get(RequestContext& ctx) {
  auto id = id_param(ctx);
  if (!id) {
    co_return co_await fail(ctx, id.error());
  }
  auto tx = service_.admin_get(ctx.caller, *id);
  if (!tx) {
    co_return co_await fail(ctx, tx.error());
  }
  co_await ctx.exchange.send(Response::json(http::status::ok, encode_admin_transaction(*tx)));
}

asio::awaitable<void> AdminHandlers::remove(RequestContext& ctx) {
  auto id = id_param(ctx);
  if (!id) {
    co_return co_await fail(ctx, id.error());
  }
  auto removed = service_.admin_delete(ctx.caller, *id);
  if (!removed) {
    co_return co_await fail(ctx, removed.error());
  }
  tlog_.info(kTag, "{} deleted transaction {}", ctx.caller.name, id->str());
  co_await ctx.exchange.send(Response::no_content());
}

asio::awaitable<void> AdminHandlers::retention(RequestContext& ctx) {
  auto id = id_param(ctx);
  if (!id) {
    co_return co_await fail(ctx, id.error());
  }
  auto body = co_await ctx.exchange.read_body_string(options_.max_json_bytes);
  if (!body) {
    co_return co_await fail(ctx, body.error());
  }
  auto days = decode_retention(*body);
  if (!days) {
    co_return co_await fail(ctx, days.error());
  }
  auto tx = service_.set_retention(ctx.caller, *id, *days);
  if (!tx) {
    co_return co_await fail(ctx, tx.error());
  }
  co_await ctx.exchange.send(Response::json(http::status::ok, encode_admin_transaction(*tx)));
}

asio::awaitable<void> AdminHandlers::status(RequestContext& ctx) {
  ServerStatus st{
      .instance = options_.instance,
      .version = options_.version,
      .auth_provider = options_.auth_provider,
      .started_at = started_at_,
      .now = clock_.now(),
      .peers = options_.peers,
      .transactions = {},
      .outgoing = service_.list_outgoing().size(),
      .sessions = sessions_.sessions().size(),
  };
  for (const auto& tx : service_.admin_list(ctx.caller, {})) {
    ++st.transactions[std::string{transaction::to_string(tx.state)}];
  }
  co_await ctx.exchange.send(Response::json(http::status::ok, encode_status(st)));
}

asio::awaitable<void> AdminHandlers::sessions(RequestContext& ctx) {
  co_await ctx.exchange.send(
      Response::json(http::status::ok, encode_sessions(sessions_.sessions())));
}

asio::awaitable<void> AdminHandlers::revoke_user(RequestContext& ctx) {
  const auto* user = ctx.param("user");
  if (user == nullptr) {
    co_return co_await fail(ctx, Error::make(ErrCode::internal, "route has no {user}"));
  }
  const auto count = sessions_.revoke_user(*user);
  blog_.record({.type = logging::BizEventType::sessions_revoked,
                .tx = std::nullopt,
                .actor = ctx.caller.name,
                .subject = *user});
  tlog_.info(kTag, "{} ended {} session(s) of {}", ctx.caller.name, count, *user);
  co_await ctx.exchange.send(Response::json(http::status::ok, encode_revoked(*user, count)));
}

asio::awaitable<void> AdminHandlers::log(RequestContext& ctx) {
  if (options_.business_log.empty()) {
    co_return co_await fail(ctx, Error::make(ErrCode::notfound, "no business log file"));
  }
  auto from = time_query(ctx, "from");
  if (!from) {
    co_return co_await fail(ctx, from.error());
  }
  if (!*from) {
    co_return co_await fail(
        ctx, Error::make(ErrCode::validation, "from is required (YYYY-MM-DDTHH:MM:SSZ)"));
  }
  auto to = time_query(ctx, "to");
  if (!to) {
    co_return co_await fail(ctx, to.error());
  }
  std::size_t limit = options_.default_log_lines;
  if (const auto text = ctx.query_value("limit")) {
    const auto [end, ec] = std::from_chars(text->data(), text->data() + text->size(), limit);
    if (ec != std::errc{} || end != text->data() + text->size() || limit == 0
        || limit > options_.max_log_lines) {
      co_return co_await fail(
          ctx, Error::make(ErrCode::validation,
                           std::format("limit must be 1..{}", options_.max_log_lines)));
    }
  }

  auto lines = logging::read_business_log(
      options_.business_log,
      // No `to`: up to and including the current second, so events logged
      // just before the query are not cut off by the exclusive end.
      logging::BizLogQuery{.from = **from,
                           .to = to->value_or(clock_.now() + std::chrono::seconds{1}),
                           .limit = limit});
  if (!lines) {
    co_return co_await fail(ctx, lines.error());
  }
  std::string body;
  for (const auto& line : *lines) {
    body += line;
    body += '\n';
  }
  auto res = Response::json(http::status::ok, std::move(body));
  res.content_type = "application/x-ndjson";
  co_await ctx.exchange.send(std::move(res));
}

asio::awaitable<void> AdminHandlers::fail(RequestContext& ctx, const Error& err) {
  if (ctx.exchange.responded()) {
    co_return;
  }
  const auto& req = ctx.exchange.request();
  if (status_for(err.code) == http::status::internal_server_error) {
    tlog_.error(kTag, "{} {}: {}: {} ({}:{})", sv(req.method_string()), sv(req.target()),
                common::to_string(err.code), err.detail, err.origin.file_name(), err.origin.line());
  }
  co_await ctx.exchange.send(Response::error(err));
}

}  // namespace confide::api
