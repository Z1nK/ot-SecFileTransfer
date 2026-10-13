#pragma once
#include "auth/user_directory.hpp"
#include "common/error.hpp"
#include "common/sha256.hpp"
#include "config/config.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace confide::auth {

using common::Result;

// Who sent a request, from the value of its Authorization header (FR-5,
// NFR-7). User passwords are checked by an IUserDirectory (local config or
// LDAP); peers always come from the config. No HTTP types here: `main`
// adapts this to api::IAuthenticator.
//
// Every failure is `auth` (401), except `unavailable` (503) when the user
// directory can't be reached. Wrong user name and wrong password give the
// same error, so names can't be probed.
//
// Thread-safe.
class Authenticator {
public:
  // `directory` must outlive the Authenticator.
  static Result<Authenticator> create(const config::Config& cfg, const IUserDirectory& directory);

  // "Basic base64(user:password)" -> user name, as the directory spells it.
  // Bearer tokens for users (auth.token_ttl_s) are not supported yet.
  Result<std::string> authenticate_user(std::string_view authorization) const;

  // "Bearer <token>" matched against every peer's accept_token -> that peer's
  // server name. Peers without an accept_token can't call us.
  Result<std::string> authenticate_peer(std::string_view authorization) const;

private:
  struct Peer {
    std::string name;
    common::Sha256Digest token;  // compared as digests: constant time, any length
  };

  Authenticator(const IUserDirectory& directory, std::vector<Peer> peers)
      : directory_(&directory), peers_(std::move(peers)) {}

  const IUserDirectory* directory_;
  std::vector<Peer> peers_;
};

}  // namespace confide::auth
