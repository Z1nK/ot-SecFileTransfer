#include "api/server.hpp"
#include "api/server/io.hpp"

#include <openssl/ssl.h>

#include <memory>
#include <utility>

namespace confide::api {

namespace ssl = asio::ssl;
using common::ErrCode;
using detail::fail;

Result<std::shared_ptr<ssl::context>> make_tls_context(const config::TlsCfg& cfg) {
  auto ctx = std::make_shared<ssl::context>(ssl::context::tls_server);
  ctx->set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                   ssl::context::no_sslv3 | ssl::context::no_tlsv1 |
                   ssl::context::no_tlsv1_1 | ssl::context::single_dh_use);

  boost::system::error_code ec;
  ctx->use_certificate_chain_file(cfg.cert_path.string(), ec);
  if (ec) {
    return fail(ErrCode::io, "TLS certificate " + cfg.cert_path.string() + ": " + ec.message());
  }
  ctx->use_private_key_file(cfg.key_path.string(), ssl::context::pem, ec);
  if (ec) {
    return fail(ErrCode::io, "TLS key " + cfg.key_path.string() + ": " + ec.message());
  }
  if (SSL_CTX_check_private_key(ctx->native_handle()) != 1) {
    return fail(ErrCode::validation, "TLS key does not match the certificate");
  }
  return ctx;
}

Result<ServerOptions> ServerOptions::from_config(const config::Config& cfg) {
  ServerOptions options{
      .bind = cfg.rest().bind,
      .port = cfg.rest().port,
      .threads = cfg.rest().threads,
      .request_timeout = std::chrono::milliseconds{cfg.rest().request_timeout_ms},
  };
  if (cfg.tls().enabled) {
    CFD_TRY(tls, make_tls_context(cfg.tls()));
    options.tls = std::move(tls);
  }
  return options;
}

}  // namespace confide::api
