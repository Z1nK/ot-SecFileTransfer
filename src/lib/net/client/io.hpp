#pragma once
// Internal to HttpClient: namespace aliases, constants and error helpers.
#include "net/http_client.hpp"

#include <boost/beast/core/error.hpp>
#include <boost/beast/http/status.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace confide::net::detail {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

using common::ErrCode;
using common::Error;

constexpr std::string_view kDefaultPort = "8080";
constexpr std::string_view kChecksumHeader = "X-Checksum-SHA256";

// Limit for JSON and error bodies; file bodies have none.
constexpr std::uint64_t kMaxReplyBytes = std::uint64_t{16} << 20;

// Chunk size for BodySource reads.
constexpr std::size_t kSourceChunkBytes = std::size_t{256} << 10;

// Connection attempts per request, and the wait before the second one
// (doubled before the third).
constexpr int kConnectAttempts = 3;
constexpr std::chrono::milliseconds kConnectRetryDelay{100};

inline std::unexpected<Error> fail(ErrCode code, std::string detail) {
  return std::unexpected(Error::make(code, std::move(detail)));
}

// `unavailable` with the operation and the system's message.
std::unexpected<Error> net_error(std::string_view what, const beast::error_code& ec);

// An error response: {"error":"<code>","detail":"..."} (api::error_body).
// The code named in the body wins; the status only fills in when the body
// names none.
Error server_error(http::status status, std::string_view body);

}  // namespace confide::net::detail
