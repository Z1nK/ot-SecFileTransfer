#include "api/server.hpp"

#include "api/server/io.hpp"
#include "api/server/server_impl.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/strand.hpp>

#include <algorithm>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The server is split over api/server/: tls.cpp (options, TLS context),
// connections.cpp (track/stop), serve.cpp (accept and request loop),
// dispatch.cpp (routing, auth, handler) and session_exchange.hpp.

namespace confide::api {

using tcp = asio::ip::tcp;
using common::ErrCode;
using detail::fail;
using detail::kTag;

HttpServer::HttpServer(ServerOptions options, const Router& router, const IAuthenticator& auth,
                       logging::TechLog& tlog)
    : impl_(std::make_unique<Impl>(std::move(options), router, auth, tlog)) {}

HttpServer::~HttpServer() {
  stop();
  wait();
}

Result<void> HttpServer::start() {
  auto& s = *impl_;
  {
    std::lock_guard lock{s.mu};
    if (s.started || s.stopping) {
      return fail(ErrCode::internal, "server already started or stopped");
    }
  }

  // bind is an IP literal or a host name; a name is resolved once, here, and
  // the first address of a configured family is used.
  boost::system::error_code ec;
  auto address = asio::ip::make_address(s.options.bind, ec);
  if (ec) {
    tcp::resolver resolver{s.ioc};
    const auto results = resolver.resolve(s.options.bind, "", tcp::resolver::address_configured, ec);
    if (ec || results.empty()) {
      return fail(ErrCode::validation, "bad bind address " + s.options.bind + ": " +
                                           (ec ? ec.message() : "no addresses"));
    }
    address = results.begin()->endpoint().address();
  }
  const tcp::endpoint endpoint{address, s.options.port};

  auto& acceptor = s.acceptor.emplace(asio::make_strand(s.ioc));
  // "host (10.1.2.3)" when bind was a name, for the error and listening messages.
  const auto shown = address.to_string() == s.options.bind
                         ? s.options.bind
                         : s.options.bind + " (" + address.to_string() + ")";
  const auto where = shown + ":" + std::to_string(s.options.port);
  if (acceptor.open(endpoint.protocol(), ec); ec) {
    return fail(ErrCode::io, "open " + where + ": " + ec.message());
  }
  if (acceptor.set_option(asio::socket_base::reuse_address(true), ec); ec) {
    return fail(ErrCode::io, "reuse_address " + where + ": " + ec.message());
  }
  if (acceptor.bind(endpoint, ec); ec) {
    return fail(ErrCode::io, "bind " + where + ": " + ec.message());
  }
  if (acceptor.listen(asio::socket_base::max_listen_connections, ec); ec) {
    return fail(ErrCode::io, "listen " + where + ": " + ec.message());
  }
  // Some kernels (WSL2 with mirrored networking, on a port Windows keeps
  // for itself) bind another port and address without an error; peers and
  // clients configured for the requested one would never reach us.
  const auto bound = acceptor.local_endpoint(ec);
  if (ec) {
    return fail(ErrCode::io, "local endpoint " + where + ": " + ec.message());
  }
  if ((s.options.port != 0 && bound.port() != s.options.port) || bound.address() != address) {
    return fail(ErrCode::io, std::format("bind {}: the system bound {}:{} instead; is the port "
                                         "reserved by the host?",
                                         where, bound.address().to_string(), bound.port()));
  }
  s.port = bound.port();

  asio::co_spawn(acceptor.get_executor(), s.accept_loop(), asio::detached);

  auto count = s.options.threads != 0 ? s.options.threads : std::thread::hardware_concurrency();
  count = std::max(count, 1U);
  const auto blocking_count = s.options.blocking_threads != 0
                                  ? s.options.blocking_threads
                                  : std::max(2 * std::thread::hardware_concurrency(), 4U);
  {
    std::lock_guard lock{s.mu};
    s.started = true;
    s.blocking = std::make_unique<asio::thread_pool>(blocking_count);
    for (unsigned i = 0; i < count; ++i) {
      s.threads.emplace_back([&s] { s.ioc.run(); });
    }
  }

  s.tlog.info(kTag, "listening on {}:{} ({}, {} threads, {} blocking threads)", shown,
              s.port.load(), s.options.tls ? "https" : "http", count, blocking_count);
  return {};
}

void HttpServer::stop() {
  impl_->stop();
}

void HttpServer::wait() {
  std::vector<std::thread> threads;
  {
    std::lock_guard lock{impl_->mu};
    threads.swap(impl_->threads);
  }
  for (auto& t : threads) {
    if (t.joinable()) {
      t.join();
    }
  }
  // Every coroutine has finished, so nothing is left on the blocking pool.
  std::unique_ptr<asio::thread_pool> blocking;
  {
    std::lock_guard lock{impl_->mu};
    blocking.swap(impl_->blocking);
  }
  if (blocking) {
    blocking->join();
  }
}

std::uint16_t HttpServer::port() const noexcept {
  return impl_->port.load();
}

}  // namespace confide::api
