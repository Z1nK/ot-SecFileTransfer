#pragma once
// Internal to HttpServer: the state behind HttpServer::Impl.
#include "api/server.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace confide::api {

namespace detail {

// One open connection, shared between its coroutine and stop(). Both only
// touch `stream` on the connection's strand, so stop() never races the
// coroutine and never closes a stream that is already gone.
struct Connection {
  asio::any_io_executor strand;
  beast::tcp_stream* stream = nullptr;  // null once the coroutine has finished
};

}  // namespace detail

struct HttpServer::Impl {
  using tcp = asio::ip::tcp;

  Impl(ServerOptions opts, const Router& r, const IAuthenticator& a, logging::TechLog& t)
      : options(std::move(opts)), router(r), auth(a), tlog(t) {}

  // serve.cpp
  asio::awaitable<void> accept_loop();
  asio::awaitable<void> serve(tcp::socket socket);

  template <class Stream>
  asio::awaitable<void> serve_requests(Stream& stream);

  // dispatch.cpp: routes one request, authenticates the caller from
  // `source` (client IP) and runs the handler.
  asio::awaitable<void> dispatch(Exchange& ex, const std::string& source);

  // connections.cpp
  // Registers a connection for stop(). nullptr if the server is stopping:
  // the caller must then close the connection itself.
  std::shared_ptr<detail::Connection> track(beast::tcp_stream& stream);
  void forget(const std::shared_ptr<detail::Connection>& conn);

  // Closes the acceptor and every connection; the coroutines then finish on
  // their own and io_context::run() returns once no work is left. Nothing is
  // abandoned in a suspended frame, so ~io_context has nothing to destroy.
  void stop();

  // Declared first, destroyed last: every socket, timer and coroutine frame
  // belongs to it.
  asio::io_context ioc;
  ServerOptions options;
  const Router& router;
  const IAuthenticator& auth;
  logging::TechLog& tlog;

  std::optional<tcp::acceptor> acceptor;
  std::atomic<std::uint16_t> port{0};

  std::mutex mu;  // guards everything below
  bool started = false;
  bool stopping = false;
  std::vector<std::thread> threads;
  std::vector<std::shared_ptr<detail::Connection>> connections;
};

}  // namespace confide::api
