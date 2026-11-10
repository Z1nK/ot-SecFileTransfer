#pragma once
#include "api/authenticator.hpp"
#include "api/router.hpp"
#include "logging/tech_log.hpp"

namespace confide::api {

// Login and logout for user tokens (FR-5). The password is checked by the
// server before the handler runs (Access::password); the handlers only turn
// the result into a token.
class AuthHandlers {
public:
  AuthHandlers(const IAuthenticator& auth, logging::TechLog& tlog) : auth_(auth), tlog_(tlog) {}

  // Adds the routes below to `router`; the handlers must outlive it.
  Result<void> register_routes(Router& router);

  // POST /auth/token (Basic auth only)  -> 200 + {"token","user","expires_at"}
  asio::awaitable<void> login(RequestContext& ctx);

  // DELETE /auth/token (Bearer token)   -> 204, the token is no longer valid
  asio::awaitable<void> logout(RequestContext& ctx);

private:
  const IAuthenticator& auth_;
  logging::TechLog& tlog_;
};

}  // namespace confide::api
