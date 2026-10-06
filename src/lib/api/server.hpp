#pragma once
#include "api/authenticator.hpp"
#include "api/router.hpp"
#include "config/config.hpp"
#include "logging/tech_log.hpp"

#include <boost/asio/ssl/context.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace confide::api {

struct ServerOptions {
  std::string bind = "0.0.0.0";
  std::uint16_t port = 8080;            // 0 = any free port (tests), see HttpServer::port()
  std::uint32_t threads = 0;            // 0 = hardware_concurrency
  std::chrono::milliseconds request_timeout{30'000};  // idle time while reading or writing
  std::uint64_t max_upload_bytes = 0;   // 0 = no limit
  std::uint32_t max_header_bytes = 16 * 1024;

  // nullptr = plain HTTP. Shared with nothing else; built by make_tls_context().
  std::shared_ptr<asio::ssl::context> tls{};

  // bind, port, threads and timeout from [rest]; TLS from [tls] when enabled.
  // `io` if the certificate or key can't be loaded.
  static Result<ServerOptions> from_config(const config::Config& cfg);
};

// Server-side TLS 1.2+ context from cert_path / key_path (FR-11). `io` if
// a file can't be read, `validation` if the key does not match the cert.
Result<std::shared_ptr<asio::ssl::context>> make_tls_context(const config::TlsCfg& cfg);

// HTTP/1.1 server on Boost.Asio/Beast (Architecture §10): one io_context
// run by a fixed thread pool, one coroutine per connection, keep-alive.
//
// For each request the session reads only the header, matches it in the
// Router, authenticates the caller for the route's Access, and calls the
// handler, which pulls the body through Exchange. Errors before the handler
// runs are answered here: 400 bad request line, 401 (with
// WWW-Authenticate), 404, 405, 413 header too large.
class HttpServer {
public:
  HttpServer(ServerOptions options, const Router& router, const IAuthenticator& auth,
             logging::TechLog& tlog);

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  HttpServer(HttpServer&&) = delete;
  HttpServer& operator=(HttpServer&&) = delete;

  // Calls stop() and wait().
  ~HttpServer();

  // Binds, listens and starts the threads; returns once the server accepts
  // connections. `validation` on a malformed bind address, `io` if it can't
  // be bound, `internal` if already started or stopped.
  Result<void> start();

  // Stops accepting and closes open connections; requests in progress fail
  // at their next read or write, and unfinished uploads are dropped by
  // storage. Returns at once. Thread-safe, idempotent, callable from a
  // signal handler's strand (main, SIGINT/SIGTERM). The services the
  // handlers use must outlive wait().
  void stop();

  // Blocks until every connection has finished and every server thread has
  // exited. Without stop() it blocks for as long as the server runs. Must
  // not be called from a handler.
  void wait();

  // Port actually bound, useful with ServerOptions::port = 0. 0 before start().
  std::uint16_t port() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace confide::api
