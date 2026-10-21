#include "session.hpp"

#include "password.hpp"

#include <openssl/crypto.h>

#include <iostream>
#include <utility>

namespace confide::cli {

Result<net::Login> login(HttpClient& client, const std::string& user, const TokenCache& cache) {
  CFD_TRY(password, read_password(user));
  auto reply = client.login({.user = user, .password = password});
  OPENSSL_cleanse(password.data(), password.size());
  if (!reply) {
    return std::unexpected(std::move(reply).error());
  }
  CFD_TRYV(cache.save(reply->token));
  return reply;
}

Result<Session> open_session(const Global& global) {
  if (global.server.empty()) {
    return fail(ErrCode::validation, "no server: use --server or FTC_SERVER");
  }
  if (global.user.empty()) {
    return fail(ErrCode::validation, "no user: use --user or FTC_USER");
  }
  CFD_TRY(url, net::ServerUrl::parse(global.server));
  // Without TLS anyone on the network can read the password and the token.
  // Checked before the password is asked for.
  if (!url.tls) {
    if (!global.allow_http) {
      return fail(ErrCode::validation,
                  "refusing to send credentials over plain http: use an https:// URL "
                  "(or --allow-http for local testing)");
    }
    std::cerr << "ftc: warning: plain http, the password and token are sent unencrypted\n";
  }
  CFD_TRY(cache, TokenCache::for_server(url, global.user));
  CFD_TRY(client, HttpClient::create(std::move(url), {.ca_file = global.ca_file, .user_agent = "ftc"}));
  return Session{.client = std::move(client), .cache = std::move(cache)};
}

Result<HttpClient> connect(const Global& global) {
  CFD_TRY(session, open_session(global));
  auto& [client, cache] = session;
  CFD_TRY(saved, cache.load());
  if (saved) {
    client.set_token(*saved);
  } else {
    CFD_TRYV(login(client, global.user, cache));
  }
  if (can_ask_password()) {
    client.on_expired([user = global.user, cache](HttpClient& c) -> Result<std::string> {
      std::cerr << "ftc: session expired, log in again\n";
      CFD_TRY(reply, login(c, user, cache));
      return reply.token;
    });
  }
  return std::move(client);
}

}  // namespace confide::cli
