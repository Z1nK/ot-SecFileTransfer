#pragma once
#include "api/authenticator.hpp"
#include "api/json.hpp"
#include "api/router.hpp"
#include "common/time.hpp"
#include "logging/business_log.hpp"
#include "logging/tech_log.hpp"
#include "transaction/service.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace confide::api {

struct AdminOptions {
  // The business log file, for GET /admin/log; empty = not available.
  std::filesystem::path business_log{};
  std::size_t default_log_lines = 1000;
  std::size_t max_log_lines = 10000;
  std::size_t max_json_bytes = std::size_t{64} * 1024;
  // What GET /admin/status reports besides the live counts.
  std::string instance{};
  std::string version{};
  std::string auth_provider{};
  std::vector<PeerView> peers{};
};

// The /admin/* routes, for members of auth.admin_group (Access::admin,
// checked by the server before a handler runs). Admins see metadata of every
// transaction and can delete one or change its retention, but never read
// file contents. Every change is written to the business log with the admin
// as actor.
class AdminHandlers {
public:
  // `clock` gives "now" for status and the log query; everything must
  // outlive the handlers.
  AdminHandlers(transaction::TransactionService& service, const IAuthenticator& auth,
                const IAdminSessions& sessions, logging::BusinessLog& blog, logging::TechLog& tlog,
                const common::IClock& clock, AdminOptions options);

  // Adds the routes below to `router`.
  Result<void> register_routes(Router& router);

  // POST /admin/token (Basic auth only)     -> 200 + {"token","user","expires_at"}
  asio::awaitable<void> login(RequestContext& ctx);

  // DELETE /admin/token                     -> 204
  asio::awaitable<void> logout(RequestContext& ctx);

  // GET /admin/transactions?user=&state=&from=&to=   -> 200 + list
  // `user` matches sender or target; from/to bound created_at, ISO-8601.
  asio::awaitable<void> list(RequestContext& ctx);

  // GET /admin/transactions/{id}            -> 200 + transaction with files
  asio::awaitable<void> get(RequestContext& ctx);

  // DELETE /admin/transactions/{id}         -> 204
  asio::awaitable<void> remove(RequestContext& ctx);

  // PUT /admin/transactions/{id}/retention {"days":N} -> 200 + transaction
  asio::awaitable<void> retention(RequestContext& ctx);

  // GET /admin/status                       -> 200 + status
  asio::awaitable<void> status(RequestContext& ctx);

  // GET /admin/sessions                     -> 200 + session list
  asio::awaitable<void> sessions(RequestContext& ctx);

  // DELETE /admin/sessions/{user}           -> 200 + {"user","revoked"}
  asio::awaitable<void> revoke_user(RequestContext& ctx);

  // GET /admin/log?from=&to=&limit=         -> 200 + application/x-ndjson
  // Business-log lines with from <= ts < to (no `to` = up to now, inclusive), oldest
  // first, at most `limit` (default_log_lines, up to max_log_lines).
  asio::awaitable<void> log(RequestContext& ctx);

private:
  // Response::error(); logs 5xx errors to the technical log.
  asio::awaitable<void> fail(RequestContext& ctx, const common::Error& err);

  transaction::TransactionService& service_;
  const IAuthenticator& auth_;
  const IAdminSessions& sessions_;
  logging::BusinessLog& blog_;
  logging::TechLog& tlog_;
  const common::IClock& clock_;
  AdminOptions options_;
  common::Timestamp started_at_;
};

}  // namespace confide::api
