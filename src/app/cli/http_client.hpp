#pragma once
#include <common/error.hpp>
#include <common/sha256.hpp>

#include <boost/beast/http/verb.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace confide::cli {

using common::Result;

// Base URL of a server: "http://host[:port]" or "https://host[:port]".
// The port defaults to 8080, like rest.port on the server.
struct ServerUrl {
  bool tls = false;
  std::string host;
  std::string port;

  // `validation` on another scheme, a missing host or a path after the port.
  static Result<ServerUrl> parse(std::string_view text);
};

struct Credentials {
  std::string user;
  std::string password;
};

// Blocking HTTP/1.1 client for the REST API (Architecture §8), with HTTP
// Basic auth. One connection per call, so there is no connection state to
// keep in sync with the server.
//
// Errors: a network or TLS failure is `unavailable`; an error response gives
// the ErrCode named in its {"error","detail"} body.
class HttpClient {
public:
  // `ca_file`: PEM bundle to verify an https server; system CAs if unset.
  static Result<HttpClient> create(ServerUrl url, const Credentials& creds,
                                   const std::optional<std::filesystem::path>& ca_file);

  HttpClient(HttpClient&&) noexcept;
  HttpClient& operator=(HttpClient&&) noexcept;
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;
  ~HttpClient();

  // Sends `json` (if not empty) and returns the response body.
  Result<std::string> call(boost::beast::http::verb method, std::string_view target,
                           std::string json = {});

  // PUT `file` as the body, streamed, with its digest in X-Checksum-SHA256.
  // Returns the response body (the stored file entry).
  Result<std::string> upload(std::string_view target, const std::filesystem::path& file,
                             const common::Sha256Digest& sha256);

  // GET `target` and stream the body into `out` (created or truncated).
  Result<void> download(std::string_view target, const std::filesystem::path& out);

private:
  struct Impl;
  explicit HttpClient(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

// Percent-encodes `path` for a request target; '/' and unreserved
// characters (RFC 3986) are left as they are.
std::string url_encode_path(std::string_view path);

}  // namespace confide::cli
