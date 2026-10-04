#pragma once
#include "common/error.hpp"
#include "storage/storage.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/beast/http.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace confide::api {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

using common::Result;

// Request line and headers. The body is not part of it: it is pulled in
// chunks through Exchange, so a 10 GB upload never sits in memory (NFR-4).
using RequestHeader = http::request_header<>;

// Header carrying the client's SHA-256 of an uploaded file, and the stored
// digest on a download.
inline constexpr std::string_view kChecksumHeader = "X-Checksum-SHA256";

// HTTP status for each error code, as documented in common/error.hpp.
http::status status_for(common::ErrCode code);

// {"error":"<to_string(code)>","detail":"..."}. Body of every error response.
std::string error_body(const common::Error& err);

// A small response with an in-memory body: JSON or an error.
struct Response {
  http::status status = http::status::ok;
  std::string body{};
  std::string content_type = "application/json";
  std::vector<std::pair<std::string, std::string>> headers{};  // extra headers

  static Response json(http::status status, std::string body);
  static Response error(const common::Error& err);
  static Response no_content();
};

// One HTTP request/response pair, the same for plain TCP and TLS. The server
// session implements it; handler tests use a fake. Handlers read the body
// through it and must send exactly one response.
//
// Network failures come back as `unavailable`: the connection is gone and
// the handler should just return.
class Exchange {
public:
  Exchange() = default;
  Exchange(const Exchange&) = delete;
  Exchange& operator=(const Exchange&) = delete;
  Exchange(Exchange&&) = delete;
  Exchange& operator=(Exchange&&) = delete;
  virtual ~Exchange() = default;

  virtual const RequestHeader& request() const = 0;

  // Content-Length of the request, nullopt for a chunked body.
  virtual std::optional<std::uint64_t> content_length() const = 0;

  // Reads the next part of the request body into `buf`; 0 at end of body.
  // `validation` if the body grows past ServerOptions::max_upload_bytes.
  virtual asio::awaitable<Result<std::size_t>> read_body(std::span<std::byte> buf) = 0;

  // Reads the whole body as text, for JSON requests. `validation` if it is
  // larger than `max_bytes`.
  virtual asio::awaitable<Result<std::string>> read_body_string(std::size_t max_bytes) = 0;

  virtual asio::awaitable<Result<void>> send(Response response) = 0;

  // Streams a stored file as application/octet-stream with Content-Length =
  // file.size() and the X-Checksum-SHA256 header, in storage::kChunkBytes
  // chunks.
  virtual asio::awaitable<Result<void>> send_file(storage::Download& file,
                                                  const common::Sha256Digest& sha256) = 0;

  // True once send() or send_file() has started; the server sends a 500
  // for handlers that return without responding.
  virtual bool responded() const noexcept = 0;
};

}  // namespace confide::api
