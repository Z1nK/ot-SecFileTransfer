#pragma once
// Internal to HttpServer: the Exchange a connection hands to handlers.
#include "api/server/io.hpp"

#include <boost/beast/core.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace confide::api::detail {

// Exchange over one parsed request header. The body is read lazily through
// the same parser, so a handler decides how and whether to consume it.
template <class Stream>
class SessionExchange final : public Exchange {
public:
  SessionExchange(Stream& stream, beast::flat_buffer& buffer, Parser& parser,
                  const ServerOptions& options)
      : stream_(stream), buffer_(buffer), parser_(parser), options_(options) {}

  const RequestHeader& request() const override { return parser_.get().base(); }

  // Client IP address, "" if the socket no longer knows it.
  std::string remote_address() const {
    boost::system::error_code ec;
    const auto ep = beast::get_lowest_layer(stream_).socket().remote_endpoint(ec);
    return ec ? std::string{} : ep.address().to_string();
  }

  std::optional<std::uint64_t> content_length() const override {
    const auto len = parser_.content_length();
    if (!len) {
      return std::nullopt;
    }
    return *len;
  }

  asio::awaitable<Result<std::size_t>> read_body(std::span<std::byte> buf) override {
    using common::ErrCode;
    if (close_) {
      co_return fail(ErrCode::unavailable, "connection closed");
    }
    if (!co_await send_continue()) {
      co_return fail(ErrCode::unavailable, "write of 100-continue failed");
    }
    while (!parser_.is_done()) {
      auto& body = parser_.get().body();
      body.data = buf.data();
      body.size = buf.size();

      set_timeout(stream_, options_.request_timeout);
      auto [ec, n] = co_await http::async_read(stream_, buffer_, parser_, kTok);
      if (ec == http::error::need_buffer) {
        ec = {};
      }
      if (ec == http::error::body_limit) {
        close_ = true;
        co_return fail(ErrCode::validation, "request body too large");
      }
      if (ec) {
        close_ = true;
        co_return fail(ErrCode::unavailable, "reading request body: " + ec.message());
      }
      const std::size_t got = buf.size() - body.size;
      if (got > 0) {
        co_return got;
      }
    }
    co_return std::size_t{0};
  }

  asio::awaitable<Result<std::string>> read_body_string(std::size_t max_bytes) override {
    using common::ErrCode;
    const auto len = content_length();
    if (len && *len > max_bytes) {
      co_return fail(ErrCode::validation, "request body too large");
    }
    std::string out;
    std::vector<std::byte> buf(std::min<std::size_t>(max_bytes + 1, 64 * 1024));
    while (true) {
      auto n = co_await read_body(buf);
      if (!n) {
        co_return std::unexpected(std::move(n).error());
      }
      if (*n == 0) {
        break;
      }
      if (out.size() + *n > max_bytes) {
        close_ = true;  // the rest of the body stays unread
        co_return fail(ErrCode::validation, "request body too large");
      }
      out.append(reinterpret_cast<const char*>(buf.data()), *n);  // NOLINT
    }
    co_return out;
  }

  asio::awaitable<Result<void>> send(Response response) override {
    responded_ = true;
    if (!co_await write_response(stream_, options_, request().version(), keep_alive(),
                                 std::move(response))) {
      close_ = true;
      co_return fail(common::ErrCode::unavailable, "writing response failed");
    }
    co_return Result<void>{};
  }

  asio::awaitable<Result<void>> send_file(storage::Download& file,
                                          const common::Sha256Digest& sha256) override {
    using common::ErrCode;
    responded_ = true;

    http::response<http::buffer_body> res{http::status::ok, request().version()};
    res.set(http::field::server, kServerName);
    res.set(http::field::content_type, "application/octet-stream");
    res.set(kChecksumHeader, sha256.to_hex());
    res.content_length(file.size());
    res.keep_alive(keep_alive());
    res.body().data = nullptr;
    res.body().more = true;

    http::response_serializer<http::buffer_body> sr{res};
    set_timeout(stream_, options_.request_timeout);
    if (auto [ec, n] = co_await http::async_write_header(stream_, sr, kTok); ec) {
      close_ = true;
      co_return fail(ErrCode::unavailable, "writing response header: " + ec.message());
    }

    std::vector<std::byte> buf(storage::kChunkBytes);
    std::uint64_t sent = 0;
    while (true) {
      auto n = file.read(buf);
      if (!n) {
        // Headers are out, so no error response is possible: drop the
        // connection and let the client notice the short body.
        close_ = true;
        co_return std::unexpected(std::move(n).error());
      }
      sent += *n;
      if (*n == 0 && sent != file.size()) {
        close_ = true;
        co_return fail(ErrCode::io, "file shrank while sending");
      }
      res.body().data = *n == 0 ? nullptr : buf.data();
      res.body().size = *n;
      res.body().more = *n != 0;

      set_timeout(stream_, options_.request_timeout);
      auto [ec, written] = co_await http::async_write(stream_, sr, kTok);
      if (ec == http::error::need_buffer) {
        ec = {};
      }
      if (ec) {
        close_ = true;
        co_return fail(ErrCode::unavailable, "writing response body: " + ec.message());
      }
      if (*n == 0) {
        break;
      }
    }
    co_return Result<void>{};
  }

  bool responded() const noexcept override { return responded_; }

  // True when the connection can't carry another request.
  bool must_close() const noexcept { return close_ || !keep_alive(); }

private:
  // Keep-alive only if the client wants it and the body was fully read;
  // otherwise unread body bytes would be taken for the next request.
  bool keep_alive() const { return !close_ && parser_.get().keep_alive() && parser_.is_done(); }

  // Answers "Expect: 100-continue" before the first body read, so curl
  // starts sending at once instead of waiting a second.
  asio::awaitable<bool> send_continue() {
    if (continue_sent_ || parser_.is_done()) {
      co_return true;
    }
    continue_sent_ = true;
    if (!beast::iequals(request()[http::field::expect], "100-continue")) {
      co_return true;
    }
    http::response<http::empty_body> res{http::status::continue_, request().version()};
    set_timeout(stream_, options_.request_timeout);
    auto [ec, n] = co_await http::async_write(stream_, res, kTok);
    co_return !ec;
  }

  Stream& stream_;
  beast::flat_buffer& buffer_;
  Parser& parser_;
  const ServerOptions& options_;
  bool responded_ = false;
  bool continue_sent_ = false;
  bool close_ = false;
};

}  // namespace confide::api::detail
