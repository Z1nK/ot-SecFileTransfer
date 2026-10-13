#include "http_client.hpp"

#include <auth/base64.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>

#include <charconv>
#include <cstdint>
#include <format>
#include <utility>

namespace confide::cli {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace json = boost::json;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

using common::ErrCode;
using common::Error;

constexpr std::string_view kDefaultPort = "8080";
constexpr std::string_view kChecksumHeader = "X-Checksum-SHA256";

// Limit for JSON and error bodies; file bodies have none.
constexpr std::uint64_t kMaxReplyBytes = std::uint64_t{16} << 20;

std::unexpected<Error> fail(ErrCode code, std::string detail) {
  return std::unexpected(Error::make(code, std::move(detail)));
}

std::unexpected<Error> net_error(std::string_view what, const beast::error_code& ec) {
  return fail(ErrCode::unavailable, std::format("{}: {}", what, ec.message()));
}

// Fallback when the error body does not name a known code.
ErrCode code_for(http::status status) {
  switch (status) {
  case http::status::bad_request:
    return ErrCode::validation;
  case http::status::unauthorized:
    return ErrCode::auth;
  case http::status::forbidden:
    return ErrCode::forbidden;
  case http::status::not_found:
    return ErrCode::notfound;
  case http::status::method_not_allowed:
  case http::status::conflict:
    return ErrCode::conflict;
  case http::status::gone:
    return ErrCode::gone;
  case http::status::unprocessable_entity:
    return ErrCode::integrity;
  case http::status::service_unavailable:
    return ErrCode::unavailable;
  default:
    return ErrCode::internal;
  }
}

// An error response: {"error":"<code>","detail":"..."} (api::error_body).
Error server_error(http::status status, std::string_view body) {
  ErrCode code = code_for(status);
  std::string detail = std::format("HTTP {}", static_cast<unsigned>(status));

  boost::system::error_code ec;
  const json::value v = json::parse(body, ec);
  if (ec || !v.is_object()) {
    return Error::make(code, std::move(detail));
  }
  const auto& obj = v.get_object();
  if (const auto* name = obj.if_contains("error"); name != nullptr && name->is_string()) {
    for (auto c = ErrCode::validation; c <= ErrCode::internal;
         c = static_cast<ErrCode>(static_cast<int>(c) + 1)) {
      if (common::to_string(c) == name->get_string()) {
        code = c;
        break;
      }
    }
  }
  if (const auto* text = obj.if_contains("detail"); text != nullptr && text->is_string()) {
    detail = std::string{text->get_string()};
  }
  return Error::make(code, std::move(detail));
}

// Reads a whole response with a small in-memory body.
template <class Stream, class Parser>
Result<std::string> finish_reply(Stream& stream, beast::flat_buffer& buf, Parser& parser) {
  parser.body_limit(kMaxReplyBytes);
  beast::error_code ec;
  http::read(stream, buf, parser, ec);
  if (ec) {
    return net_error("receive", ec);
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
  beast::error_code write_ec;
  http::write(stream, req, write_ec);

  beast::flat_buffer buf;
  http::response_parser<http::string_body> parser;
  auto reply = finish_reply(stream, buf, parser);
  if (write_ec && (reply || reply.error().code == ErrCode::unavailable)) {
    return net_error("send", write_ec);
  }
  return reply;
}

}  // namespace

Result<ServerUrl> ServerUrl::parse(std::string_view text) {
  ServerUrl url;
  if (text.starts_with("https://")) {
    url.tls = true;
    text.remove_prefix(8);
  } else if (text.starts_with("http://")) {
    text.remove_prefix(7);
  } else {
    return fail(ErrCode::validation, "server URL must start with http:// or https://");
  }
  if (text.ends_with('/')) {
    text.remove_suffix(1);
  }
  if (text.find('/') != std::string_view::npos) {
    return fail(ErrCode::validation, "server URL must not have a path");
  }

  const auto colon = text.rfind(':');
  url.host = std::string{text.substr(0, colon)};
  url.port = colon == std::string_view::npos ? std::string{kDefaultPort}
                                             : std::string{text.substr(colon + 1)};
  if (url.host.empty()) {
    return fail(ErrCode::validation, "server URL has no host");
  }
  unsigned port = 0;
  const auto* end = url.port.data() + url.port.size();
  const auto [ptr, ec] = std::from_chars(url.port.data(), end, port);
  if (ec != std::errc{} || ptr != end || port == 0 || port > 65535) {
    return fail(ErrCode::validation, "bad port in server URL: " + url.port);
  }
  return url;
}

struct HttpClient::Impl {
  ServerUrl url;
  std::string authorization;
  std::unique_ptr<ssl::context> tls;  // null for http://
  asio::io_context io;

  template <class Body>
  http::request<Body> make_request(http::verb method, std::string_view target) const {
    http::request<Body> req{method, target, 11};
    req.set(http::field::host, url.host + ":" + url.port);
    req.set(http::field::user_agent, "ftc");
    req.set(http::field::authorization, authorization);
    req.keep_alive(false);
    return req;
  }

  // Connects, runs `fn(stream)` on a plain or TLS stream, and closes.
  template <class T, class Fn>
  Result<T> exchange(Fn&& fn) {
    beast::error_code ec;
    tcp::resolver resolver(io);
    const auto endpoints = resolver.resolve(url.host, url.port, ec);
    if (ec) {
      return net_error("resolve " + url.host, ec);
    }

    if (!tls) {
      beast::tcp_stream stream(io);
      stream.connect(endpoints, ec);
      if (ec) {
        return net_error("connect " + url.host + ":" + url.port, ec);
      }
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
    beast::get_lowest_layer(stream).connect(endpoints, ec);
    if (ec) {
      return net_error("connect " + url.host + ":" + url.port, ec);
    }
    stream.handshake(ssl::stream_base::client, ec);
    if (ec) {
      return net_error("TLS handshake", ec);
    }
    auto res = fn(stream);
    stream.shutdown(ec);  // the server may just close; nothing to do about it
    return res;
  }
};

HttpClient::HttpClient(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
HttpClient::HttpClient(HttpClient&&) noexcept = default;
HttpClient& HttpClient::operator=(HttpClient&&) noexcept = default;
HttpClient::~HttpClient() = default;

Result<HttpClient> HttpClient::create(ServerUrl url, const Credentials& creds,
                                      const std::optional<std::filesystem::path>& ca_file) {
  auto impl = std::make_unique<Impl>();
  const std::string pair = creds.user + ":" + creds.password;
  impl->authorization = "Basic "
                        + auth::base64_encode(common::ByteSpan{
                            reinterpret_cast<const std::byte*>(pair.data()),  // NOLINT
                            pair.size()});

  if (url.tls) {
    impl->tls = std::make_unique<ssl::context>(ssl::context::tls_client);
    impl->tls->set_verify_mode(ssl::verify_peer);
    beast::error_code ec;
    if (ca_file) {
      impl->tls->load_verify_file(ca_file->string(), ec);
    } else {
      impl->tls->set_default_verify_paths(ec);
    }
    if (ec) {
      return fail(ErrCode::validation, "CA certificates: " + ec.message());
    }
  }
  impl->url = std::move(url);
  return HttpClient(std::move(impl));
}

Result<std::string> HttpClient::call(http::verb method, std::string_view target, std::string json) {
  return impl_->exchange<std::string>([&](auto& stream) {
    auto req = impl_->make_request<http::string_body>(method, target);
    if (!json.empty()) {
      req.set(http::field::content_type, "application/json");
      req.body() = std::move(json);
    }
    req.prepare_payload();
    return roundtrip(stream, req);
  });
}

Result<std::string> HttpClient::upload(std::string_view target, const std::filesystem::path& file,
                                       const common::Sha256Digest& sha256) {
  return impl_->exchange<std::string>([&](auto& stream) -> Result<std::string> {
    auto req = impl_->make_request<http::file_body>(http::verb::put, target);
    req.set(http::field::content_type, "application/octet-stream");
    req.set(kChecksumHeader, sha256.to_hex());
    beast::error_code ec;
    req.body().open(file.c_str(), beast::file_mode::scan, ec);
    if (ec) {
      return fail(ErrCode::io, std::format("{}: {}", file.string(), ec.message()));
    }
    req.prepare_payload();
    return roundtrip(stream, req);
  });
}

Result<void> HttpClient::download(std::string_view target, const std::filesystem::path& out) {
  return impl_->exchange<void>([&](auto& stream) -> Result<void> {
    auto req = impl_->make_request<http::empty_body>(http::verb::get, target);
    beast::error_code ec;
    http::write(stream, req, ec);
    if (ec) {
      return net_error("send", ec);
    }

    // Header first: only a 200 body goes to the file.
    beast::flat_buffer buf;
    http::response_parser<http::empty_body> head;
    http::read_header(stream, buf, head, ec);
    if (ec) {
      return net_error("receive", ec);
    }
    if (head.get().result() != http::status::ok) {
      http::response_parser<http::string_body> parser{std::move(head)};
      auto reply = finish_reply(stream, buf, parser);
      if (!reply) {
        return std::unexpected(std::move(reply).error());
      }
      return fail(ErrCode::internal,
                  std::format("unexpected HTTP {}", static_cast<unsigned>(parser.get().result())));
    }

    http::response_parser<http::file_body> parser{std::move(head)};
    parser.body_limit(boost::none);
    parser.get().body().open(out.c_str(), beast::file_mode::write, ec);
    if (ec) {
      return fail(ErrCode::io, std::format("{}: {}", out.string(), ec.message()));
    }
    http::read(stream, buf, parser, ec);
    parser.get().body().close();
    if (ec) {
      return net_error("receive", ec);
    }
    return {};
  });
}

std::string url_encode_path(std::string_view path) {
  constexpr std::string_view kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(path.size());
  for (const char c : path) {
    const auto u = static_cast<unsigned char>(c);
    const bool keep = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9')
                      || c == '-' || c == '.' || c == '_' || c == '~' || c == '/';
    if (keep) {
      out.push_back(c);
    } else {
      out.push_back('%');
      out.push_back(kHex[u >> 4]);
      out.push_back(kHex[u & 0x0F]);
    }
  }
  return out;
}

}  // namespace confide::cli
