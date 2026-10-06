#pragma once
#include "api/http.hpp"
#include "transaction/service.hpp"

namespace confide::api {

// Turns the Authorization header into a transaction::Caller. Implemented on
// top of the `auth` module (UserStore, token check) and wired in `main`, so
// api can be tested with a fake.
//
// Both return `auth` when the header is missing or wrong (401, FR-5).
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
  virtual Result<transaction::Caller> authenticate_user(const RequestHeader& req) const = 0;

  // Access::peer routes: "Bearer <peer token>", checked against each peer's
  // accept_token. Caller::name is the peer's server name, is_peer is true.
  virtual Result<transaction::Caller> authenticate_peer(const RequestHeader& req) const = 0;
};

}  // namespace confide::api
