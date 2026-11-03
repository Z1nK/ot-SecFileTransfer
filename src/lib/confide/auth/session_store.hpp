#pragma once
#include "common/error.hpp"
#include "common/sha256.hpp"
#include "common/time.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace confide::auth {

using common::Result;

// What a token may be used for. Admin tokens come from POST /admin/token and
// are accepted on /admin/* only; user tokens everywhere else.
enum class Scope : std::uint8_t { user, admin };

std::string_view to_string(Scope scope);

// A token handed out by POST /auth/token or POST /admin/token.
struct IssuedToken {
  std::string token;  // "ftu_"/"fta_" + 64 hex digits; only the client keeps it
  common::Timestamp expires_at;
};

struct SessionOptions {
  std::chrono::seconds ttl{3600};  // auth.token_ttl_s: lifetime from login
  std::chrono::seconds idle{900};  // auth.token_idle_s: unused this long = gone
  std::size_t max_per_user = 20;   // per scope; a new login past this drops the oldest
  std::chrono::seconds admin_ttl{3600};  // auth.admin_token_ttl_s
  std::chrono::seconds admin_idle{900};  // auth.admin_token_idle_s
};

// One live session as GET /admin/sessions shows it; never the token.
struct SessionInfo {
  std::string user;
  Scope scope = Scope::user;
  common::Timestamp created;
  common::Timestamp last_used;
  common::Timestamp expires_at;
};

// User sessions behind Bearer tokens (FR-5). Tokens are 32 random bytes;
// only their SHA-256 is kept, so a memory dump does not hand out sessions.
// In memory only: a server restart logs everybody out.
//
// Thread-safe.
class SessionStore {
public:
  // `clock` must outlive the store.
  SessionStore(const common::IClock& clock, SessionOptions options)
      : clock_(&clock), options_(options) {}

  // New session for `user` (canonical name, already authenticated).
  // `internal` if the random generator fails.
  Result<IssuedToken> issue(std::string_view user, Scope scope = Scope::user);

  // Token -> user name, and marks the session as used. `auth` for an
  // unknown, revoked or expired token, or one of another scope.
  Result<std::string> verify(std::string_view token, Scope scope = Scope::user);

  // Ends the session of `token`; unknown tokens are ignored.
  void revoke(std::string_view token);

  // Ends every session of `user`, both scopes. Returns how many there were.
  std::size_t revoke_user(std::string_view user);

  // Live sessions, oldest login first.
  std::vector<SessionInfo> list();

  // Number of live sessions, for tests.
  std::size_t size() const;

  // Prefixes of user and admin tokens, so they can't be mistaken for a peer
  // token or for each other.
  static constexpr std::string_view kPrefix = "ftu_";
  static constexpr std::string_view kAdminPrefix = "fta_";

  static std::string_view prefix(Scope scope) {
    return scope == Scope::admin ? kAdminPrefix : kPrefix;
  }

private:
  struct Session {
    std::string user;
    Scope scope = Scope::user;
    common::Timestamp created;
    common::Timestamp last_used;
    common::Timestamp expires_at;
  };

  bool expired(const Session& s, common::Timestamp now) const;
  void drop_expired(common::Timestamp now);
  void drop_oldest_of(std::string_view user, Scope scope);

  const common::IClock* clock_;
  SessionOptions options_;
  mutable std::mutex mutex_;
  std::unordered_map<common::Sha256Digest, Session> sessions_;
};

}  // namespace confide::auth
