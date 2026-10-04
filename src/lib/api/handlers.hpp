#pragma once
#include "api/router.hpp"
#include "logging/business_log.hpp"
#include "logging/tech_log.hpp"
#include "transaction/service.hpp"

#include <cstddef>

namespace confide::api {

struct HandlerOptions {
  std::size_t max_json_bytes = std::size_t{64} * 1024;  // limit for JSON request bodies
};

// Translates REST calls into TransactionService calls and errors into status
// codes (Architecture §8). No business rules here: state and access checks
// are the service's job.
//
// `forbidden` from the service is answered as 404, so outsiders can't probe
// which transaction ids exist (NFR-7).
class TransactionHandlers {
public:
  TransactionHandlers(transaction::TransactionService& service, logging::BusinessLog& blog,
                      logging::TechLog& tlog, HandlerOptions options = {});

  // Adds every route below to `router`; the handlers must outlive it.
  Result<void> register_routes(Router& router);

  // --- Users (Access::user) -----------------------------------------------

  // POST /transactions                     -> 201 + transaction
  asio::awaitable<void> create(RequestContext& ctx);

  // GET /transactions?box=in|out           -> 200 + list (box defaults to in)
  asio::awaitable<void> list(RequestContext& ctx);

  // GET /transactions/{id}                 -> 200 + transaction
  asio::awaitable<void> get(RequestContext& ctx);

  // PUT /transactions/{id}/files/{path*}   -> 201 + file entry
  // Streams the body into PendingUpload in storage::kChunkBytes chunks and
  // checks the optional X-Checksum-SHA256 header (`integrity` -> 422).
  asio::awaitable<void> upload(RequestContext& ctx);

  // DELETE /transactions/{id}/files/{path*} -> 204
  asio::awaitable<void> remove_file(RequestContext& ctx);

  // POST /transactions/{id}/commit         -> 200 + transaction
  asio::awaitable<void> commit(RequestContext& ctx);

  // GET /transactions/{id}/files           -> 200 + file list
  asio::awaitable<void> list_files(RequestContext& ctx);

  // GET /transactions/{id}/files/{path*}   -> 200 + streamed file
  // Logs `downloaded` only after the whole file was sent.
  asio::awaitable<void> download(RequestContext& ctx);

  // --- Peers (Access::peer, Architecture §7) -------------------------------

  // POST /internal/transactions            -> 201 + transaction
  asio::awaitable<void> receive(RequestContext& ctx);

  // PUT  /internal/transactions/{id}/files/{path*} and
  // POST /internal/transactions/{id}/commit use upload() and commit(): the
  // Caller carries is_peer, and the service applies the peer rules.

private:
  // Parses the {id} / {path*} parameters; `validation` (400) if malformed.
  static Result<common::TransactionId> id_param(const RequestContext& ctx);
  static Result<common::RelativePath> path_param(const RequestContext& ctx);

  // Response::error() with `forbidden` turned into `notfound`; logs 5xx
  // errors to the technical log.
  asio::awaitable<void> fail(RequestContext& ctx, const common::Error& err);

  transaction::TransactionService& service_;
  logging::BusinessLog& blog_;
  logging::TechLog& tlog_;
  HandlerOptions options_;
};

}  // namespace confide::api
