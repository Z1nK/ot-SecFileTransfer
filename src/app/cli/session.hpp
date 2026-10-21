#pragma once
// Connecting to the server: URL checks, token file, login.
#include "cli.hpp"
#include "token_cache.hpp"

#include <net/http_client.hpp>

#include <string>

namespace confide::cli {

using net::HttpClient;

// Client and token file for --server and --user, with no token set yet.
struct Session {
  HttpClient client;
  TokenCache cache;
};

// Refuses plain http unless --allow-http, before any password is asked for.
Result<Session> open_session(const Global& global);

// Asks for the password, trades it for a token and saves the token.
Result<net::Login> login(HttpClient& client, const std::string& user, const TokenCache& cache);

// A client that sends the saved token, or logs in first if there is none.
// Logs in again when the token expires, if a password can be asked for.
Result<HttpClient> connect(const Global& global);

}  // namespace confide::cli
