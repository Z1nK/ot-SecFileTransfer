#pragma once
#include "common/error.hpp"
#include "common/sha256.hpp"

#include <boost/beast/http/verb.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace confide::net {

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

// Answer to POST /auth/token.
struct Login {
  std::string token;
  std::string user;        // as the server spells it
  std::string expires_at;  // ISO 8601, UTC
};

struct ClientOptions {
  // PEM bundle to verify an https server; system CAs if unset.
  std::optional<std::filesystem::path> ca_file{};
  // Longest wait for a single connect, read or write; 0 = no limit. An idle
  // limit, not a total one: a long transfer is fine as long as bytes move.
  std::chrono::milliseconds io_timeout{0};
  std::string user_agent = "confide";
};

// A request body pulled in chunks, for upload() from something that is not
// a plain file. `read` fills the buffer and returns how many bytes it wrote;
// it must deliver exactly `size` bytes in total.
struct BodySource {
  std::uint64_t size = 0;
  std::function<Result<std::size_t>(std::span<std::byte>)> read;
};

// Blocking HTTP/1.1 client for the REST API (Architecture §8). Used by `ftc`
// with a user token and by the forwarder with a peer token, both sent as
// "Bearer". One connection per call, so there is no connection state to
// keep in sync with the server.
//
// Errors: a network or TLS failure, a timeout or cancel() is `unavailable`;
// an error response gives the ErrCode named in its {"error","detail"} body.
class HttpClient {
public:
  // Called when the server rejects the token (`auth`): returns a new token,
  // usually from login(), or an error that ends the request.
  using Relogin = std::function<Result<std::string>(HttpClient&)>;

  // `validation` if the CA bundle can't be loaded.
  static Result<HttpClient> create(ServerUrl url, ClientOptions options = {});

  // POST /auth/token with the password (Basic); the only request that sends
  // it. On success the client uses the new token from then on.
  Result<Login> login(const Credentials& creds);

  // DELETE /auth/token: the server forgets the current token.
  Result<void> logout();

  void set_token(std::string_view token);

  // With a Relogin set, a request rejected with `auth` gets one retry with
  // the token it returns.
  void on_expired(Relogin relogin);

  // Aborts the request in progress, and every later one, with `unavailable`.
  // The only call that may come from another thread.
  void cancel();

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

  // Same, with the body pulled from `source`. Not retried after a relogin,
  // since the source can't be rewound; an error from `source` itself is
  // returned as it is.
  Result<std::string> upload(std::string_view target, BodySource source,
                             const common::Sha256Digest& sha256);

  // GET `target` and stream the body into `out` (created or truncated).
  Result<void> download(std::string_view target, const std::filesystem::path& out);

private:
  struct Impl;

  // Runs `attempt`, and once more after a relogin if it failed with `auth`.
  template <class T, class Fn>
  Result<T> with_relogin(Fn&& attempt);

  explicit HttpClient(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

// Percent-encodes `path` for a request target; '/' and unreserved
// characters (RFC 3986) are left as they are.
std::string url_encode_path(std::string_view path);

}  // namespace confide::net
