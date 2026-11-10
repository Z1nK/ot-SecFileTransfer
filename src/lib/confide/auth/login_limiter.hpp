#pragma once
#include "common/error.hpp"
#include "common/time.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace confide::auth {

using common::Result;

struct LoginLimiterOptions {
  // Failures allowed before the backoff starts, per key. The pair limit is
  // the one a user with typos meets; the others stop guessing spread over
  // many names (per address) or many addresses (per user).
  std::uint32_t free_per_pair = 5;     // same user from the same address
  std::uint32_t free_per_source = 20;  // any user from one address
  std::uint32_t free_per_user = 50;    // one user from any address
  // Wait after the first failure past the limit; doubles each time.
  std::chrono::seconds base_delay{30};
  std::chrono::seconds max_delay{900};
  // Past this many keys, keys idle for max_delay are dropped.
  std::size_t max_keys = 10'000;
};

// Slows down password guessing on Basic credentials (FR-5, NFR-7). While a
// key is in backoff the password is not checked at all, so the user
// directory never sees the attempt: with LDAP, an attacker can't use this
// server to run an account into the directory's own lockout faster than the
// backoff allows.
//
// `source` is the client address; user names are compared lowercase.
//
// Thread-safe.
class LoginLimiter {
public:
  // `clock` must outlive the limiter.
  LoginLimiter(const common::IClock& clock, LoginLimiterOptions options = {})
      : clock_(&clock), options_(options) {}

  // `throttled` while `user` or `source` is in backoff.
  Result<void> check(std::string_view user, std::string_view source);

  void failed(std::string_view user, std::string_view source);

  // Clears the user's counters; the address keeps its count, so one known
  // password does not reset guessing on other names.
  void succeeded(std::string_view user, std::string_view source);

private:
  struct Entry {
    std::uint32_t failures = 0;
    common::Timestamp last_failure{};
    common::Timestamp blocked_until{};
  };

  void bump(const std::string& key, std::uint32_t free, common::Timestamp now);
  void drop_idle(common::Timestamp now);

  const common::IClock* clock_;
  LoginLimiterOptions options_;
  std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
};

}  // namespace confide::auth
