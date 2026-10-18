#include "api/server/io.hpp"
#include "api/server/server_impl.hpp"
#include "api/server/session_exchange.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/ssl.hpp>

#include <exception>
#include <utility>

namespace confide::api {

namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using common::ErrCode;
using common::Error;
using detail::kTag;
using detail::kTok;
using detail::set_timeout;
using detail::write_response;

// GCC false positive: the temporary produced by `co_await` lives in the coroutine frame and
// GCC cannot prove it is initialized when the frame is torn down.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
asio::awaitable<void> HttpServer::Impl::accept_loop() {
  while (acceptor->is_open()) {
    auto [ec, socket] = co_await acceptor->async_accept(asio::make_strand(ioc), kTok);
    if (ec == asio::error::operation_aborted || !acceptor->is_open()) {
      co_return;
    }
    if (ec) {
      tlog.warn(kTag, "accept failed: {}", ec.message());
      continue;
    }
    auto exec = socket.get_executor();
    asio::co_spawn(exec, serve(std::move(socket)), asio::detached);
  }
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

asio::awaitable<void> HttpServer::Impl::serve(tcp::socket socket) {
  beast::tcp_stream stream{std::move(socket)};
  const auto conn = track(stream);
  if (!conn) {
    co_return;  // stopping; the socket closes with `stream`
  }

  try {
    if (!options.tls) {
      co_await serve_requests(stream);
      boost::system::error_code ec;
      stream.socket().shutdown(tcp::socket::shutdown_send, ec);
    } else {
      // `stream` is moved into the TLS stream; track its lowest layer instead.
      ssl::stream<beast::tcp_stream> tls_stream{std::move(stream), *options.tls};
      conn->stream = &beast::get_lowest_layer(tls_stream);
      set_timeout(tls_stream, options.request_timeout);
      auto [ec] = co_await tls_stream.async_handshake(ssl::stream_base::server, kTok);
      if (ec) {
        tlog.debug(kTag, "TLS handshake failed: {}", ec.message());
      } else {
        co_await serve_requests(tls_stream);
        set_timeout(tls_stream, options.request_timeout);
        co_await tls_stream.async_shutdown(kTok);
      }
    }
  } catch (const std::exception& e) {
    tlog.error(kTag, "connection aborted: {}", e.what());
  }
  // No suspension since the stream's last use, so stop() can't see a
  // dangling pointer in between.
  forget(conn);
}

template <class Stream>
asio::awaitable<void> HttpServer::Impl::serve_requests(Stream& stream) {
  beast::flat_buffer buffer;
  while (true) {
    detail::Parser parser;
    parser.header_limit(options.max_header_bytes);
    if (options.max_upload_bytes != 0) {
      parser.body_limit(options.max_upload_bytes);
    } else {
      parser.body_limit(boost::none);
    }

    set_timeout(stream, options.request_timeout);
    auto [ec, n] = co_await http::async_read_header(stream, buffer, parser, kTok);
    if (ec == http::error::end_of_stream || ec == asio::error::eof) {
      co_return;
    }
    if (ec == http::error::header_limit) {
      co_await write_response(stream, options, 11, false,
                              Response{
                                  .status = http::status::payload_too_large,
                                  .body = error_body(Error::make(ErrCode::validation,
                                                                 "request header too large")),
                              });
      co_return;
    }
    if (ec == beast::error::timeout || ec == asio::error::operation_aborted) {
      co_return;
    }
    if (ec) {
      // Malformed request line or headers; connection errors end here too,
      // where the write simply fails.
      tlog.debug(kTag, "bad request: {}", ec.message());
      co_await write_response(stream, options, 11, false,
                              Response::error(Error::make(ErrCode::validation, ec.message())));
      co_return;
    }

    detail::SessionExchange<Stream> ex{stream, buffer, parser, options};
    co_await dispatch(ex, ex.remote_address());
    if (ex.must_close()) {
      co_return;
    }
  }
}

}  // namespace confide::api
