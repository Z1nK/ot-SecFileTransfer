#pragma once
#include "api/http.hpp"
#include "common/time.hpp"
#include "transaction/service.hpp"

#include <string>
#include <string_view>

namespace confide::api {

// A user token handed out by POST /auth/token.
struct IssuedToken {
  std::string token;
  common::Timestamp expires_at;
};

// Turns the Authorization header into a transaction::Caller, and issues and
// revokes user tokens. Implemented on top of the `auth` module (user
// directory, SessionStore, LoginLimiter) and wired in `main`, so api can be
// tested with a fake.
//
// `source` is the client address, for limiting password guesses.
//
// The authenticate_* calls return `auth` when the header is missing or wrong
// (401, FR-5), `throttled` (429) after too many wrong passwords.
class IAuthenticator {
public:
  IAuthenticator() = default;
  IAuthenticator(const IAuthenticator&) = delete;
  IAuthenticator& operator=(const IAuthenticator&) = delete;
  IAuthenticator(IAuthenticator&&) = delete;
  IAuthenticator& operator=(IAuthenticator&&) = delete;
  virtual ~IAuthenticator() = default;

  // Access::user routes: "Basic <user:password>" or "Bearer <token>".
  // Caller::is_peer is false.
  virtual Result<transaction::Caller> authenticate_user(const RequestHeader& req,
                                                        std::string_view source) const = 0;

  // Access::password routes: "Basic <user:password>" only.
  virtual Result<transaction::Caller> authenticate_password(const RequestHeader& req,
                                                            std::string_view source) const = 0;

  // Access::peer routes: "Bearer <peer token>", checked against each peer's
  // accept_token. Caller::name is the peer's server name, is_peer is true.
  virtual Result<transaction::Caller> authenticate_peer(const RequestHeader& req) const = 0;

  // New token for `user`, who has just passed authenticate_password().
  virtual Result<IssuedToken> issue_token(std::string_view user) const = 0;

  // Ends the session of the request's "Bearer <token>", if it has one.
  virtual void revoke_token(const RequestHeader& req) const = 0;
};

}  // namespace confide::api
