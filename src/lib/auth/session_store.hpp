#pragma once
#include "common/error.hpp"
#include "common/sha256.hpp"
#include "common/time.hpp"

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace confide::auth {

using common::Result;

// A user token handed out by POST /auth/token.
struct IssuedToken {
  std::string token;  // "ftu_" + 64 hex digits; only the client keeps it
  common::Timestamp expires_at;
};

struct SessionOptions {
  std::chrono::seconds ttl{3600};  // auth.token_ttl_s: lifetime from login
  std::chrono::seconds idle{900};  // auth.token_idle_s: unused this long = gone
  std::size_t max_per_user = 20;   // a new login past this drops the oldest
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
  Result<IssuedToken> issue(std::string_view user);

  // Token -> user name, and marks the session as used. `auth` for an
  // unknown, revoked or expired token.
  Result<std::string> verify(std::string_view token);

  // Ends the session of `token`; unknown tokens are ignored.
  void revoke(std::string_view token);

  // Number of live sessions, for tests.
  std::size_t size() const;

  // Prefix of every user token, so it can't be mistaken for a peer token.
  static constexpr std::string_view kPrefix = "ftu_";

private:
  struct Session {
    std::string user;
    common::Timestamp created;
    common::Timestamp last_used;
    common::Timestamp expires_at;
  };

  bool expired(const Session& s, common::Timestamp now) const;
  void drop_expired(common::Timestamp now);
  void drop_oldest_of(std::string_view user);

  const common::IClock* clock_;
  SessionOptions options_;
  mutable std::mutex mutex_;
  std::unordered_map<common::Sha256Digest, Session> sessions_;
};

}  // namespace confide::auth
