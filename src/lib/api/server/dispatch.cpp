#include "api/server/io.hpp"
#include "api/server/server_impl.hpp"

#include <exception>
#include <string>
#include <utility>

namespace confide::api {

using common::ErrCode;
using common::Error;
using detail::kTag;
using detail::sv;

asio::awaitable<void> HttpServer::Impl::dispatch(Exchange& ex, const std::string& source) {
  const auto& req = ex.request();

  auto match = router.match(req.method(), sv(req.target()));
  if (!match) {
    auto res = Response::error(match.error());
    if (match.error().code == ErrCode::conflict) {
      res.status = http::status::method_not_allowed;
    }
    co_await ex.send(std::move(res));
    co_return;
  }

  const Route& route = *match->route;
  auto caller = [&] {
    switch (route.access) {
    case Access::peer:
      return auth.authenticate_peer(req);
    case Access::password:
      return auth.authenticate_password(req, source);
    case Access::user:
      break;
    }
    return auth.authenticate_user(req, source);
  }();
  if (!caller) {
    if (caller.error().code == ErrCode::throttled) {
      tlog.warn(kTag, "{} {} from {}: {}", sv(req.method_string()), sv(req.target()), source,
                caller.error().detail);
    }
    auto res = Response::error(caller.error());
    if (caller.error().code == ErrCode::auth) {
      res.headers.emplace_back("WWW-Authenticate", route.access == Access::peer
                                                       ? R"(Bearer realm="confide-peer")"
                                                       : R"(Basic realm="confide")");
    }
    co_await ex.send(std::move(res));
    co_return;
  }

  RequestContext ctx{
      .exchange = ex,
      .caller = std::move(*caller),
      .params = std::move(match->params),
      .query = std::move(match->query),
  };

  std::string what;
  try {
    co_await route.handler(ctx);
  } catch (const std::exception& e) {
    what = e.what();
  }

  if (!what.empty() || !ex.responded()) {
    if (what.empty()) {
      what = "handler sent no response";
    }
    tlog.error(kTag, "{} {}: {}", sv(req.method_string()), sv(req.target()), what);
    if (!ex.responded()) {
      co_await ex.send(Response::error(Error::make(ErrCode::internal, "internal error")));
    }
  }
}

}  // namespace confide::api
