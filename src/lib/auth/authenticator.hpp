#pragma once
#include "auth/login_limiter.hpp"
#include "auth/session_store.hpp"
#include "auth/user_directory.hpp"
#include "common/error.hpp"
#include "common/sha256.hpp"
#include "common/time.hpp"
#include "config/config.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace confide::auth {

using common::Result;

// Who sent a request, from the value of its Authorization header (FR-5,
// NFR-7). User passwords are checked by an IUserDirectory (local config or
// LDAP); user tokens by a SessionStore; peers always come from the config.
// No HTTP types here: `main` adapts this to api::IAuthenticator.
//
// Every failure is `auth` (401), except `unavailable` (503) when the user
// directory can't be reached, `throttled` (429) after too many wrong
// passwords (LoginLimiter) and `forbidden` (403) for a valid user who is
// not in auth.admin_group on an admin call. Wrong user name and wrong password give the
// same error, so names can't be probed.
//
// `source` is the client address, for the LoginLimiter.
//
// Thread-safe.
class Authenticator {
public:
  // `directory` and `clock` must outlive the Authenticator.
  static Result<Authenticator> create(const config::Config& cfg, const IUserDirectory& directory,
                                      const common::IClock& clock, LoginLimiterOptions limits = {});

  // "Basic base64(user:password)" or "Bearer <user token>" -> user name, as
  // the directory spells it.
  Result<std::string> authenticate_user(std::string_view authorization,
                                        std::string_view source) const;

  // Basic only, for POST /auth/token: a token must not buy a new token, or
  // a stolen one would never expire.
  Result<std::string> authenticate_password(std::string_view authorization,
                                            std::string_view source) const;

  // "Bearer <token>" matched against every peer's accept_token -> that peer's
  // server name. Peers without an accept_token can't call us. User tokens
  // are never accepted here.
  Result<std::string> authenticate_peer(std::string_view authorization) const;

  // "Basic ..." or "Bearer <admin token>" of a member of auth.admin_group
  // -> user name. Membership is asked of the directory on every call, so
  // removing someone from the group ends their admin rights without waiting
  // for their token to expire. `forbidden` if they are not a member, and
  // always when no admin_group is configured.
  Result<std::string> authenticate_admin(std::string_view authorization,
                                         std::string_view source) const;

  // Basic only, for POST /admin/token; same membership check.
  Result<std::string> authenticate_admin_password(std::string_view authorization,
                                                  std::string_view source) const;

  // A session for `user`, who has just passed authenticate_password() (user
  // scope) or authenticate_admin_password() (admin scope).
  Result<IssuedToken> issue_token(std::string_view user, Scope scope = Scope::user) const;

  // Ends the session of a "Bearer <token>" header; anything else is ignored.
  void revoke_token(std::string_view authorization) const;

  // For the admin API: every live session, and logging a user out
  // everywhere. revoke_user returns how many sessions ended.
  std::vector<SessionInfo> sessions() const { return sessions_->list(); }
  std::size_t revoke_user(std::string_view user) const { return sessions_->revoke_user(user); }

private:
  struct Peer {
    std::string name;
    common::Sha256Digest token;  // compared as digests: constant time, any length
  };

  Authenticator(const IUserDirectory& directory, std::string admin_group, std::vector<Peer> peers,
                std::unique_ptr<SessionStore> sessions, std::unique_ptr<LoginLimiter> limiter)
      : directory_(&directory)
      , admin_group_(std::move(admin_group))
      , peers_(std::move(peers))
      , sessions_(std::move(sessions))
      , limiter_(std::move(limiter)) {}

  // `forbidden` unless `user` is in admin_group_.
  Result<std::string> require_admin(std::string user) const;

  const IUserDirectory* directory_;
  std::string admin_group_;  // empty = nobody is admin
  std::vector<Peer> peers_;
  // Behind pointers: they hold mutexes, and the Authenticator is moved.
  std::unique_ptr<SessionStore> sessions_;
  std::unique_ptr<LoginLimiter> limiter_;
};

}  // namespace confide::auth
