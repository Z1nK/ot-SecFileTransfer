#include "auth/login_limiter.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <utility>

namespace confide::auth {

namespace {

using common::ErrCode;
using common::Error;
using common::Timestamp;

std::string lower(std::string_view s) {
  std::string out{s};
  std::ranges::transform(out, out.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// The prefix keeps the three kinds apart. A Basic user name never holds a
// ':' (the header is split at the first one), so in the pair key the
// address, which may (IPv6), can safely come last.
std::string user_key(std::string_view user) {
  return "u:" + lower(user);
}
std::string source_key(std::string_view source) {
  return "s:" + std::string{source};
}
std::string pair_key(std::string_view user, std::string_view source) {
  return "p:" + lower(user) + ':' + std::string{source};
}

}  // namespace

Result<void> LoginLimiter::check(std::string_view user, std::string_view source) {
  const std::array keys{pair_key(user, source), source_key(source), user_key(user)};

  const std::scoped_lock lock(mutex_);
  const Timestamp now = clock_->now();
  Timestamp until{};
  for (const auto& key : keys) {
    if (const auto it = entries_.find(key); it != entries_.end()) {
      until = std::max(until, it->second.blocked_until);
    }
  }
  if (until > now) {
    return std::unexpected(
        Error::make(ErrCode::throttled,
                    std::format("too many failed logins, retry in {} s", (until - now).count())));
  }
  return {};
}

void LoginLimiter::bump(const std::string& key, std::uint32_t free, Timestamp now) {
  auto& e = entries_[key];
  ++e.failures;
  e.last_failure = now;
  if (e.failures <= free) {
    return;
  }
  const auto steps = std::min<std::uint32_t>(e.failures - free - 1, 20);  // no shift overflow
  const auto delay = std::min(options_.base_delay * (std::int64_t{1} << steps), options_.max_delay);
  e.blocked_until = now + delay;
}

void LoginLimiter::drop_idle(Timestamp now) {
  std::erase_if(entries_, [&](const auto& kv) {
    return kv.second.blocked_until <= now && kv.second.last_failure + options_.max_delay <= now;
  });
}

void LoginLimiter::failed(std::string_view user, std::string_view source) {
  const std::scoped_lock lock(mutex_);
  const Timestamp now = clock_->now();
  if (entries_.size() >= options_.max_keys) {
    drop_idle(now);
  }
  bump(pair_key(user, source), options_.free_per_pair, now);
  bump(source_key(source), options_.free_per_source, now);
  bump(user_key(user), options_.free_per_user, now);
}

void LoginLimiter::succeeded(std::string_view user, std::string_view source) {
  const std::scoped_lock lock(mutex_);
  entries_.erase(pair_key(user, source));
  entries_.erase(user_key(user));
}

}  // namespace confide::auth
