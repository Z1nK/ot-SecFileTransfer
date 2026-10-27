#pragma once
// Connecting to the server: URL checks, token file, login.
#include "cli.hpp"
#include "token_cache.hpp"

#include <net/http_client.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace confide::cli {

using net::HttpClient;

// Which token a session uses: the user token of /auth/token, or the admin
// token of /admin/token (`ftc admin ...`), kept in separate files.
enum class Scope : std::uint8_t { user, admin };

// Client and token file for --server and --user, with no token set yet.
struct Session {
  HttpClient client;
  TokenCache cache;
  Scope scope = Scope::user;
};

// Refuses plain http unless --allow-http, before any password is asked for.
Result<Session> open_session(const Global& global, Scope scope = Scope::user);

// Asks for the password, trades it for a token of `scope` and saves the token.
Result<net::Login> login(HttpClient& client, const std::string& user, const TokenCache& cache,
                         Scope scope = Scope::user);

// POST/DELETE target of the tokens of `scope`.
std::string_view token_target(Scope scope);

// A client that sends the saved token, or logs in first if there is none.
// Logs in again when the token expires, if a password can be asked for.
Result<HttpClient> connect(const Global& global, Scope scope = Scope::user);

}  // namespace confide::cli
