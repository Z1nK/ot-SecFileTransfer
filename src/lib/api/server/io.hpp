#pragma once
// Internal to HttpServer: shared constants and small I/O helpers.
#include "api/http.hpp"
#include "api/server.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>

#include <chrono>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace confide::api::detail {

constexpr auto kTok = asio::as_tuple(asio::use_awaitable);
constexpr std::string_view kServerName = "confide";
constexpr std::string_view kTag = "api";

using Parser = http::request_parser<http::buffer_body>;

inline std::unexpected<common::Error> fail(common::ErrCode code, std::string detail) {
  return std::unexpected(common::Error::make(code, std::move(detail)));
}

inline std::string_view sv(beast::string_view s) {
  return {s.data(), s.size()};
}

template <class Stream>
void set_timeout(Stream& stream, std::chrono::milliseconds timeout) {
  beast::get_lowest_layer(stream).expires_after(timeout);
}

// Writes a small response outside of a handler (errors before routing).
template <class Stream>
asio::awaitable<bool> write_response(Stream& stream, const ServerOptions& options,
                                     unsigned version, bool keep_alive, Response response) {
  http::response<http::string_body> res{response.status, version};
  res.set(http::field::server, kServerName);
  if (!response.content_type.empty()) {
    res.set(http::field::content_type, response.content_type);
  }
  for (const auto& [name, value] : response.headers) {
    res.set(name, value);
  }
  res.body() = std::move(response.body);
  res.keep_alive(keep_alive);
  res.prepare_payload();

  set_timeout(stream, options.request_timeout);
  auto [ec, n] = co_await http::async_write(stream, res, kTok);
  co_return !ec;
}

}  // namespace confide::api::detail
