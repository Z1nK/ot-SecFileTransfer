#include "net/http_client.hpp"

#include "common/base64.hpp"
#include "net/client/client_impl.hpp"
#include "net/client/io.hpp"
#include "net/client/source_body.hpp"

#include <openssl/crypto.h>

#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <boost/optional.hpp>

#include <format>
#include <utility>

// The client is split over net/client/: io (constants, errors),
// source_body.hpp (upload from a BodySource) and client_impl.hpp (connect,
// timeouts, retries, read/write); ServerUrl and url_encode_path are in
// url.cpp.

namespace confide::net {

namespace json = boost::json;

HttpClient::HttpClient(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
HttpClient::HttpClient(HttpClient&&) noexcept = default;
HttpClient& HttpClient::operator=(HttpClient&&) noexcept = default;
HttpClient::~HttpClient() = default;

Result<HttpClient> HttpClient::create(ServerUrl url, ClientOptions options) {
  auto impl = std::make_unique<Impl>();
  if (url.tls) {
    impl->tls = std::make_unique<ssl::context>(ssl::context::tls_client);
    impl->tls->set_verify_mode(ssl::verify_peer);
    beast::error_code ec;
    if (options.ca_file) {
      impl->tls->load_verify_file(options.ca_file->string(), ec);
    } else {
      impl->tls->set_default_verify_paths(ec);
    }
    if (ec) {
      return fail(ErrCode::validation, "CA certificates: " + ec.message());
    }
  }
  impl->url = std::move(url);
  impl->options = std::move(options);
  return HttpClient(std::move(impl));
}

void HttpClient::set_token(std::string_view token) {
  impl_->authorization = "Bearer " + std::string{token};
}

void HttpClient::on_expired(Relogin relogin) {
  impl_->relogin = std::move(relogin);
}

void HttpClient::cancel() {
  impl_->cancelled = true;
  impl_->io.stop();
}

template <class T, class Fn>
Result<T> HttpClient::with_relogin(Fn&& attempt) {
  auto res = attempt();
  if (res || res.error().code != ErrCode::auth || !impl_->relogin) {
    return res;
  }
  CFD_TRY(token, impl_->relogin(*this));
  set_token(token);
  return attempt();
}

Result<Login> HttpClient::login(const Credentials& creds) {
  std::string pair = creds.user + ":" + creds.password;
  std::string basic = "Basic "
                      + common::base64_encode(common::ByteSpan{
                          reinterpret_cast<const std::byte*>(pair.data()),  // NOLINT
                          pair.size()});
  auto reply = impl_->exchange<std::string>([&](auto& stream) {
    auto req = impl_->make_request<http::empty_body>(http::verb::post, "/auth/token");
    req.set(http::field::authorization, basic);
    req.prepare_payload();
    return impl_->roundtrip(stream, req);
  });
  // Best effort: copies inside Beast and the caller's string are not ours.
  OPENSSL_cleanse(pair.data(), pair.size());
  OPENSSL_cleanse(basic.data(), basic.size());
  if (!reply) {
    return std::unexpected(std::move(reply).error());
  }

  boost::system::error_code ec;
  const json::value v = json::parse(*reply, ec);
  const auto* obj = ec ? nullptr : v.if_object();
  const auto text = [&](std::string_view key) -> std::string {
    const auto* f = obj == nullptr ? nullptr : obj->if_contains(key);
    return f != nullptr && f->is_string() ? std::string{f->get_string()} : std::string{};
  };
  Login out{.token = text("token"), .user = text("user"), .expires_at = text("expires_at")};
  if (out.token.empty()) {
    return fail(ErrCode::internal, "server sent no token");
  }
  set_token(out.token);
  return out;
}

Result<void> HttpClient::logout() {
  return impl_
      ->exchange<std::string>([&](auto& stream) {
        auto req = impl_->make_request<http::empty_body>(http::verb::delete_, "/auth/token");
        req.prepare_payload();
        return impl_->roundtrip(stream, req);
      })
      .transform([](const std::string&) {});
}

Result<std::string> HttpClient::call(http::verb method, std::string_view target, std::string json) {
  return with_relogin<std::string>([&] {
    return impl_->exchange<std::string>([&](auto& stream) {
      auto req = impl_->make_request<http::string_body>(method, target);
      if (!json.empty()) {
        req.set(http::field::content_type, "application/json");
        req.body() = json;  // a copy: a retry sends it again
      }
      req.prepare_payload();
      return impl_->roundtrip(stream, req);
    });
  });
}

Result<std::string> HttpClient::upload(std::string_view target, const std::filesystem::path& file,
                                       const common::Sha256Digest& sha256) {
  return with_relogin<std::string>([&] {
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
      return impl_->roundtrip(stream, req);
    });
  });
}

Result<std::string> HttpClient::upload(std::string_view target, BodySource source,
                                       const common::Sha256Digest& sha256) {
  return impl_->exchange<std::string>([&](auto& stream) -> Result<std::string> {
    auto req = impl_->make_request<SourceBody>(http::verb::put, target);
    req.set(http::field::content_type, "application/octet-stream");
    req.set(kChecksumHeader, sha256.to_hex());
    req.body().source = std::move(source);
    req.prepare_payload();
    return impl_->roundtrip(stream, req);
  });
}

Result<void> HttpClient::download(std::string_view target, const std::filesystem::path& out) {
  return with_relogin<void>([&] {
    return impl_->exchange<void>([&](auto& stream) -> Result<void> {
      auto req = impl_->make_request<http::empty_body>(http::verb::get, target);
      http::request_serializer<http::empty_body> sr{req};
      if (const auto ec = impl_->write_all(stream, sr); ec) {
        return impl_->op_error("send", ec);
      }

      // Header first: only a 200 body goes to the file. Beast checks
      // Content-Length against the limit as soon as the header is in, so
      // the default 8 MiB would refuse a large file here; an error body
      // gets kMaxReplyBytes in finish_reply().
      beast::flat_buffer buf;
      http::response_parser<http::empty_body> head;
      head.body_limit(boost::none);
      if (const auto ec = impl_->read_all(stream, buf, head, true); ec) {
        return impl_->op_error("receive", ec);
      }
      if (head.get().result() != http::status::ok) {
        http::response_parser<http::string_body> parser{std::move(head)};
        auto reply = impl_->finish_reply(stream, buf, parser);
        if (!reply) {
          return std::unexpected(std::move(reply).error());
        }
        return fail(ErrCode::internal, std::format("unexpected HTTP {}",
                                                   static_cast<unsigned>(parser.get().result())));
      }

      http::response_parser<http::file_body> parser{std::move(head)};
      parser.body_limit(boost::none);
      beast::error_code ec;
      parser.get().body().open(out.c_str(), beast::file_mode::write, ec);
      if (ec) {
        return fail(ErrCode::io, std::format("{}: {}", out.string(), ec.message()));
      }
      ec = impl_->read_all(stream, buf, parser);
      parser.get().body().close();
      if (ec) {
        return impl_->op_error("receive", ec);
      }
      return {};
    });
  });
}

}  // namespace confide::net
