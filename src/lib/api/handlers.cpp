#include "api/handlers.hpp"

#include "api/json.hpp"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace confide::api {

namespace {

using common::ErrCode;
using common::Error;

std::string_view sv(beast::string_view s) {
  return {s.data(), s.size()};
}

}  // namespace

TransactionHandlers::TransactionHandlers(transaction::TransactionService& service,
                                         logging::BusinessLog& blog, logging::TechLog& tlog,
                                         HandlerOptions options)
    : service_(service), blog_(blog), tlog_(tlog), options_(options) {}

Result<void> TransactionHandlers::register_routes(Router& router) {
  using http::verb;
  using M = asio::awaitable<void> (TransactionHandlers::*)(RequestContext&);
  struct Def {
    verb method;
    const char* pattern;
    Access access;
    M fn;
  };
  const std::array defs{
      Def{
          .method = verb::post,
          .pattern = "/transactions",
          .access = Access::user,
          .fn = &TransactionHandlers::create,
      },
      Def{
          .method = verb::get,
          .pattern = "/transactions",
          .access = Access::user,
          .fn = &TransactionHandlers::list,
      },
      Def{
          .method = verb::get,
          .pattern = "/transactions/{id}",
          .access = Access::user,
          .fn = &TransactionHandlers::get,
      },
      Def{
          .method = verb::post,
          .pattern = "/transactions/{id}/commit",
          .access = Access::user,
          .fn = &TransactionHandlers::commit,
      },
      Def{
          .method = verb::get,
          .pattern = "/transactions/{id}/files",
          .access = Access::user,
          .fn = &TransactionHandlers::list_files,
      },
      Def{
          .method = verb::put,
          .pattern = "/transactions/{id}/files/{path*}",
          .access = Access::user,
          .fn = &TransactionHandlers::upload,
      },
      Def{
          .method = verb::get,
          .pattern = "/transactions/{id}/files/{path*}",
          .access = Access::user,
          .fn = &TransactionHandlers::download,
      },
      Def{
          .method = verb::delete_,
          .pattern = "/transactions/{id}/files/{path*}",
          .access = Access::user,
          .fn = &TransactionHandlers::remove_file,
      },
      Def{
          .method = verb::post,
          .pattern = "/internal/transactions",
          .access = Access::peer,
          .fn = &TransactionHandlers::receive,
      },
      Def{
          .method = verb::put,
          .pattern = "/internal/transactions/{id}/files/{path*}",
          .access = Access::peer,
          .fn = &TransactionHandlers::upload,
      },
      Def{
          .method = verb::post,
          .pattern = "/internal/transactions/{id}/commit",
          .access = Access::peer,
          .fn = &TransactionHandlers::commit,
      },
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

// --- Users ------------------------------------------------------------------

asio::awaitable<void> TransactionHandlers::create(RequestContext& ctx) {
  auto body = co_await ctx.exchange.read_body_string(options_.max_json_bytes);
  if (!body) {
    co_return co_await fail(ctx, body.error());
  }
  auto req = decode_create(*body);
  if (!req) {
    co_return co_await fail(ctx, req.error());
  }
  auto tx = service_.create(ctx.caller, *req);
  if (!tx) {
    co_return co_await fail(ctx, tx.error());
  }
  co_await ctx.exchange.send(Response::json(http::status::created, encode_transaction(*tx)));
}

asio::awaitable<void> TransactionHandlers::list(RequestContext& ctx) {
  const auto box_text = ctx.query_value("box").value_or("in");
  transaction::Box box{};
  if (box_text == "in") {
    box = transaction::Box::in;
  } else if (box_text == "out") {
    box = transaction::Box::out;
  } else {
    co_return co_await fail(ctx, Error::make(ErrCode::validation, "box must be 'in' or 'out'"));
  }
  const auto txs = service_.list(ctx.caller, box);
  co_await ctx.exchange.send(Response::json(http::status::ok, encode_transaction_list(txs)));
}

asio::awaitable<void> TransactionHandlers::get(RequestContext& ctx) {
  auto id = id_param(ctx);
  if (!id) {
    co_return co_await fail(ctx, id.error());
  }
  auto tx = service_.get(ctx.caller, *id);
  if (!tx) {
    co_return co_await fail(ctx, tx.error());
  }
  co_await ctx.exchange.send(Response::json(http::status::ok, encode_transaction(*tx)));
}

asio::awaitable<void> TransactionHandlers::upload(RequestContext& ctx) {
  auto id = id_param(ctx);
  if (!id) {
    co_return co_await fail(ctx, id.error());
  }
  auto path = path_param(ctx);
  if (!path) {
    co_return co_await fail(ctx, path.error());
  }

  std::optional<common::Sha256Digest> expected;
  const auto header = sv(ctx.exchange.request()[kChecksumHeader]);
  if (!header.empty()) {
    auto digest = common::Sha256Digest::from_hex(header);
    if (!digest) {
      co_return co_await fail(
          ctx, Error::make(ErrCode::validation,
                           std::string{kChecksumHeader} + ": " + digest.error().detail));
    }
    expected = *digest;
  }

  auto pending = service_.begin_upload(ctx.caller, *id, *path);
  if (!pending) {
    co_return co_await fail(ctx, pending.error());
  }

  std::vector<std::byte> buf(storage::kChunkBytes);
  while (true) {
    auto n = co_await ctx.exchange.read_body(buf);
    if (!n) {
      co_return co_await fail(ctx, n.error());
    }
    if (*n == 0) {
      break;
    }
    auto written = pending->write(common::ByteSpan{buf.data(), *n});
    if (!written) {
      co_return co_await fail(ctx, written.error());
    }
  }

  auto entry = pending->finish(expected);
  if (!entry) {
    co_return co_await fail(ctx, entry.error());
  }
  co_await ctx.exchange.send(Response::json(http::status::created, encode_file(*entry)));
}

asio::awaitable<void> TransactionHandlers::remove_file(RequestContext& ctx) {
  auto id = id_param(ctx);
  if (!id) {
    co_return co_await fail(ctx, id.error());
  }
  auto path = path_param(ctx);
  if (!path) {
    co_return co_await fail(ctx, path.error());
  }
  auto removed = service_.remove_file(ctx.caller, *id, *path);
  if (!removed) {
    co_return co_await fail(ctx, removed.error());
  }
  co_await ctx.exchange.send(Response::no_content());
}

asio::awaitable<void> TransactionHandlers::commit(RequestContext& ctx) {
  auto id = id_param(ctx);
  if (!id) {
    co_return co_await fail(ctx, id.error());
  }
  auto tx = service_.commit(ctx.caller, *id);
  if (!tx) {
    co_return co_await fail(ctx, tx.error());
  }
  co_await ctx.exchange.send(Response::json(http::status::ok, encode_transaction(*tx)));
}

asio::awaitable<void> TransactionHandlers::list_files(RequestContext& ctx) {
  auto id = id_param(ctx);
  if (!id) {
    co_return co_await fail(ctx, id.error());
  }
  auto tx = service_.get(ctx.caller, *id);
  if (!tx) {
    co_return co_await fail(ctx, tx.error());
  }
  co_await ctx.exchange.send(Response::json(http::status::ok, encode_files(tx->files)));
}

asio::awaitable<void> TransactionHandlers::download(RequestContext& ctx) {
  auto id = id_param(ctx);
  if (!id) {
    co_return co_await fail(ctx, id.error());
  }
  auto path = path_param(ctx);
  if (!path) {
    co_return co_await fail(ctx, path.error());
  }

  // open_download() applies the access and state rules; the transaction is
  // immutable once readable, so the entry read here matches the file.
  auto file = service_.open_download(ctx.caller, *id, *path);
  if (!file) {
    co_return co_await fail(ctx, file.error());
  }
  auto tx = service_.get(ctx.caller, *id);
  if (!tx) {
    co_return co_await fail(ctx, tx.error());
  }
  const auto* entry = tx->find_file(*path);
  if (entry == nullptr) {
    co_return co_await fail(ctx, Error::make(ErrCode::notfound, "no such file: " + path->str()));
  }

  auto sent = co_await ctx.exchange.send_file(*file, entry->sha256);
  if (!sent) {
    tlog_.warn("api", "download {} {} aborted: {}", id->str(), path->str(), sent.error().detail);
    co_return;
  }

  blog_.record({
      .type = logging::BizEventType::downloaded,
      .tx = *id,
      .actor = ctx.caller.name,
      .from_user = tx->sender,
      .to_user = tx->target_user,
      .to_server = tx->target_server,
      .file = path->str(),
      .size = entry->size,
      .sha256 = entry->sha256.to_hex(),
  });
}

// --- Peers ------------------------------------------------------------------

asio::awaitable<void> TransactionHandlers::receive(RequestContext& ctx) {
  auto body = co_await ctx.exchange.read_body_string(options_.max_json_bytes);
  if (!body) {
    co_return co_await fail(ctx, body.error());
  }
  auto req = decode_receive(*body);
  if (!req) {
    co_return co_await fail(ctx, req.error());
  }
  auto tx = service_.receive(ctx.caller, *req);
  if (!tx) {
    co_return co_await fail(ctx, tx.error());
  }
  co_await ctx.exchange.send(Response::json(http::status::created, encode_transaction(*tx)));
}

// --- Helpers ----------------------------------------------------------------

Result<common::TransactionId> TransactionHandlers::id_param(const RequestContext& ctx) {
  const auto* text = ctx.param("id");
  if (text == nullptr) {
    return std::unexpected(Error::make(ErrCode::internal, "route has no {id}"));
  }
  return common::TransactionId::parse(*text);
}

Result<common::RelativePath> TransactionHandlers::path_param(const RequestContext& ctx) {
  const auto* text = ctx.param("path");
  if (text == nullptr) {
    return std::unexpected(Error::make(ErrCode::internal, "route has no {path*}"));
  }
  return common::RelativePath::parse(*text);
}

asio::awaitable<void> TransactionHandlers::fail(RequestContext& ctx, const Error& err) {
  if (ctx.exchange.responded()) {
    co_return;
  }

  const auto& req = ctx.exchange.request();
  if (status_for(err.code) == http::status::internal_server_error) {
    tlog_.error("api", "{} {}: {}: {} ({}:{})", sv(req.method_string()), sv(req.target()),
                common::to_string(err.code), err.detail, err.origin.file_name(),
                err.origin.line());
  }

  if (err.code == ErrCode::forbidden) {
    co_await ctx.exchange.send(
        Response::error(Error::make(ErrCode::notfound, "no such transaction")));
    co_return;
  }
  co_await ctx.exchange.send(Response::error(err));
}

}  // namespace confide::api
