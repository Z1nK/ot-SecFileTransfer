#pragma once
#include "auth/password.hpp"
#include "common/error.hpp"
#include "common/sha256.hpp"
#include "config/config.hpp"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace confide::auth {

using common::Result;

// Who sent a request, from the value of its Authorization header (FR-5,
// NFR-7). Users and peers come from the config. No HTTP types here: `main`
// adapts this to api::IAuthenticator.
//
// Every failure is `auth` (401). Wrong user name and wrong password give the
// same error and take about the same time, so names can't be probed.
//
// Thread-safe.
class Authenticator {
public:
  // `validation` naming the user whose password_hash can't be parsed, so a
  // bad config fails at startup rather than at the first login.
  static Result<Authenticator> create(const config::Config& cfg);

  // "Basic base64(user:password)" -> user name. Bearer tokens for users
  // (auth.token_ttl_s) are not supported yet.
  //
  // PBKDF2 is slow by design, and Basic credentials come with every request.
  // So after a successful check the user's password is remembered as a
  // salted SHA-256 in memory, and the same password is accepted again
  // without PBKDF2. A different password always goes through PBKDF2.
  Result<std::string> authenticate_user(std::string_view authorization) const;

  // "Bearer <token>" matched against every peer's accept_token -> that peer's
  // server name. Peers without an accept_token can't call us.
  Result<std::string> authenticate_peer(std::string_view authorization) const;

private:
  struct Peer {
    std::string name;
    common::Sha256Digest token;  // compared as digests: constant time, any length
  };

  // Last verified password of each user, as SHA-256(salt || password).
  struct Cache {
    std::mutex mu;
    std::map<std::string, common::Sha256Digest, std::less<>> by_user;
  };

  Authenticator(std::map<std::string, PasswordHash, std::less<>> users, std::vector<Peer> peers,
                std::optional<PasswordHash> dummy)
      : users_(std::move(users))
      , peers_(std::move(peers))
      , dummy_(std::move(dummy))
      , cache_(std::make_unique<Cache>()) {}

  bool check_password(std::string_view user, const PasswordHash& hash,
                      std::string_view password) const;

  std::map<std::string, PasswordHash, std::less<>> users_;
  std::vector<Peer> peers_;
  // Verified against for unknown users, so they cost the same as known ones.
  std::optional<PasswordHash> dummy_;
  std::unique_ptr<Cache> cache_;
};

}  // namespace confide::auth
