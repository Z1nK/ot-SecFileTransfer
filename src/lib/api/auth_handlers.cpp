#include "api/auth_handlers.hpp"

#include "api/json.hpp"

namespace confide::api {

Result<void> AuthHandlers::register_routes(Router& router) {
  CFD_TRYV(router.add(Route{
      .method = http::verb::post,
      .pattern = "/auth/token",
      .access = Access::password,
      .handler = [this](RequestContext& ctx) { return login(ctx); },
  }));
  CFD_TRYV(router.add(Route{
      .method = http::verb::delete_,
      .pattern = "/auth/token",
      .access = Access::user,
      .handler = [this](RequestContext& ctx) { return logout(ctx); },
  }));
  return {};
}

asio::awaitable<void> AuthHandlers::login(RequestContext& ctx) {
  auto issued = auth_.issue_token(ctx.caller.name);
  if (!issued) {
    tlog_.error("api", "token for {}: {}", ctx.caller.name, issued.error().detail);
    co_await ctx.exchange.send(Response::error(issued.error()));
    co_return;
  }
  tlog_.info("api", "token issued for {}", ctx.caller.name);
  auto res = Response::json(http::status::ok,
                            encode_token(issued->token, ctx.caller.name, issued->expires_at));
  // The body is a credential: no proxy or client cache may keep it.
  res.headers.emplace_back("Cache-Control", "no-store");
  co_await ctx.exchange.send(std::move(res));
}

asio::awaitable<void> AuthHandlers::logout(RequestContext& ctx) {
  auth_.revoke_token(ctx.exchange.request());
  tlog_.info("api", "token revoked for {}", ctx.caller.name);
  co_await ctx.exchange.send(Response::no_content());
}

}  // namespace confide::api
