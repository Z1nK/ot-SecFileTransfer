#pragma once
// Internal to HttpClient: the state behind HttpClient::Impl, and the
// connect, read and write steps every request is built from.
#include "net/client/io.hpp"
#include "net/client/source_body.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace confide::net {

namespace detail {
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
}  // namespace detail

// Only net's own .cpp files include this header.
using namespace detail;  // NOLINT(google-build-using-namespace)

// Every network operation is started asynchronously and driven by io.run()
// on the calling thread, which makes the call blocking for the caller while
// beast::tcp_stream enforces io_timeout and cancel() can stop it.
struct HttpClient::Impl {
  ServerUrl url;
  ClientOptions options;
  std::string authorization;
  HttpClient::Relogin relogin;
  std::unique_ptr<ssl::context> tls;  // null for http://
  asio::io_context io;
  std::atomic<bool> cancelled{false};

  // Runs one asynchronous operation to completion. `start` gets the
  // completion handler.
  template <class Start>
  beast::error_code run(Start&& start) {
    beast::error_code result = asio::error::operation_aborted;
    io.restart();
    // Checked after restart(): a cancel() between the two still stops run().
    if (cancelled) {
      return result;
    }
    std::forward<Start>(start)([&result](beast::error_code ec, auto&&...) { result = ec; });
    io.run();
    // A cancel() leaves the handler queued; it is never run, since every
    // later run() returns at the check above.
    return result;
  }

  // Sets the deadline for the next operation on `stream`.
  template <class Stream>
  void arm(Stream& stream) const {
    auto& layer = beast::get_lowest_layer(stream);
    if (options.io_timeout.count() > 0) {
      layer.expires_after(options.io_timeout);
    } else {
      layer.expires_never();
    }
  }

  // Error for a failed operation; says so when cancel() was the cause.
  std::unexpected<Error> op_error(std::string_view what, const beast::error_code& ec) const {
    if (cancelled) {
      return fail(ErrCode::unavailable, std::format("{}: cancelled", what));
    }
    return net_error(what, ec);
  }

  // Writes the whole message, one write_some at a time so that io_timeout
  // limits each one rather than the whole body.
  template <class Stream, class Serializer>
  beast::error_code write_all(Stream& stream, Serializer& sr) {
    while (!sr.is_done()) {
      arm(stream);
      const auto ec =
          run([&](auto handler) { http::async_write_some(stream, sr, std::move(handler)); });
      if (ec) {
        return ec;
      }
    }
    return {};
  }

  // Reads until the parser has the header (`header_only`) or the message.
  template <class Stream, class Parser>
  beast::error_code read_all(Stream& stream, beast::flat_buffer& buf, Parser& parser,
                             bool header_only = false) {
    while (header_only ? !parser.is_header_done() : !parser.is_done()) {
      arm(stream);
      const auto ec = run(
          [&](auto handler) { http::async_read_some(stream, buf, parser, std::move(handler)); });
      if (ec) {
        return ec;
      }
    }
    return {};
  }

  template <class Body>
  http::request<Body> make_request(http::verb method, std::string_view target) const {
    http::request<Body> req{method, target, 11};
    req.set(http::field::host, url.host + ":" + url.port);
    req.set(http::field::user_agent, options.user_agent);
    if (!authorization.empty()) {
      req.set(http::field::authorization, authorization);
    }
    req.keep_alive(false);
    return req;
  }

  // Reads a whole response with a small in-memory body.
  template <class Stream, class Parser>
  Result<std::string> finish_reply(Stream& stream, beast::flat_buffer& buf, Parser& parser) {
    parser.body_limit(kMaxReplyBytes);
    if (const auto ec = read_all(stream, buf, parser); ec) {
      return op_error("receive", ec);
    }
    auto& res = parser.get();
    if (http::to_status_class(res.result()) != http::status_class::successful) {
      return std::unexpected(server_error(res.result(), res.body()));
    }
    return std::move(res.body());
  }

  // Sends `req` and reads the reply. If the server answers before taking the
  // whole body (e.g. 401 on an upload), its error wins over the write error.
  template <class Stream, class Body>
  Result<std::string> roundtrip(Stream& stream, http::request<Body>& req) {
    http::request_serializer<Body> sr{req};
    const auto write_ec = write_all(stream, sr);
    if (write_ec && cancelled) {
      return op_error("send", write_ec);
    }
    if constexpr (std::is_same_v<Body, SourceBody>) {
      // Our side failed mid-body: the server is still waiting for the rest,
      // so there is no answer to wait for.
      if (req.body().failure) {
        return std::unexpected(*req.body().failure);
      }
    }

    beast::flat_buffer buf;
    http::response_parser<http::string_body> parser;
    auto reply = finish_reply(stream, buf, parser);
    if (write_ec && (reply || reply.error().code == ErrCode::unavailable)) {
      return op_error("send", write_ec);
    }
    return reply;
  }

  // Connects, runs `fn(stream)` on a plain or TLS stream, and closes. A
  // connection that fails before `fn` sends anything (connect, TLS
  // handshake) is tried again a couple of times: nothing reached the server
  // yet, so a retry can't do anything twice, and a lone dropped connection
  // would otherwise end a long `ftc send` or a push.
  template <class T, class Fn>
  Result<T> exchange(Fn&& fn) {
    if (cancelled) {
      return fail(ErrCode::unavailable, "cancelled");
    }
    beast::error_code ec;
    tcp::resolver resolver(io);
    const auto endpoints = resolver.resolve(url.host, url.port, ec);
    if (ec) {
      return net_error("resolve " + url.host, ec);
    }
    for (int attempt = 1;; ++attempt) {
      bool reached = false;
      auto res = exchange_once<T>(endpoints, fn, reached);
      if (res || reached || cancelled || attempt == kConnectAttempts
          || res.error().code != ErrCode::unavailable) {
        return res;
      }
      std::this_thread::sleep_for(kConnectRetryDelay * attempt);
    }
  }

  // One connection for exchange(); `reached` is set once `fn` runs.
  template <class T, class Fn>
  Result<T> exchange_once(const tcp::resolver::results_type& endpoints, Fn& fn, bool& reached) {
    beast::error_code ec;
    const auto where = url.host + ":" + url.port;

    if (!tls) {
      beast::tcp_stream stream(io);
      arm(stream);
      ec = run([&](auto handler) { stream.async_connect(endpoints, std::move(handler)); });
      if (ec) {
        return op_error("connect " + where, ec);
      }
      reached = true;
      auto res = fn(stream);
      stream.socket().shutdown(tcp::socket::shutdown_both, ec);
      return res;
    }

    beast::ssl_stream<beast::tcp_stream> stream(io, *tls);
    // SNI, and the certificate must name the host we asked for.
    if (SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str()) == 0) {
      return fail(ErrCode::internal, "TLS: cannot set SNI host name");
    }
    stream.set_verify_callback(ssl::host_name_verification(url.host));
    auto& tcp_layer = beast::get_lowest_layer(stream);
    arm(stream);
    ec = run([&](auto handler) { tcp_layer.async_connect(endpoints, std::move(handler)); });
    if (ec) {
      return op_error("connect " + where, ec);
    }
    arm(stream);
    ec = run([&](auto handler) {
      stream.async_handshake(ssl::stream_base::client, std::move(handler));
    });
    if (ec) {
      return op_error("TLS handshake", ec);
    }
    reached = true;
    auto res = fn(stream);
    // The server may just close; nothing to do about an error here.
    arm(stream);
    run([&](auto handler) { stream.async_shutdown(std::move(handler)); });
    return res;
  }
};

}  // namespace confide::net
