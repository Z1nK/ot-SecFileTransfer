#include "auth/session_store.hpp"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace confide::auth {

namespace {

using common::ErrCode;
using common::Error;
using common::Sha256;
using common::Timestamp;

constexpr std::size_t kTokenBytes = 32;

std::unexpected<Error> denied(std::string detail) {
  return std::unexpected(Error::make(ErrCode::auth, std::move(detail)));
}

std::string to_hex(const std::array<unsigned char, kTokenBytes>& bytes) {
  constexpr std::string_view kHex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const unsigned char b : bytes) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

}  // namespace

std::string_view to_string(Scope scope) {
  return scope == Scope::admin ? "admin" : "user";
}

bool SessionStore::expired(const Session& s, Timestamp now) const {
  const auto idle = s.scope == Scope::admin ? options_.admin_idle : options_.idle;
  return common::is_expired(s.expires_at, now) || common::is_expired(s.last_used + idle, now);
}

void SessionStore::drop_expired(Timestamp now) {
  std::erase_if(sessions_, [&](const auto& kv) { return expired(kv.second, now); });
}

void SessionStore::drop_oldest_of(std::string_view user, Scope scope) {
  std::size_t count = 0;
  auto oldest = sessions_.end();
  for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
    if (it->second.user != user || it->second.scope != scope) {
      continue;
    }
    ++count;
    if (oldest == sessions_.end() || it->second.created < oldest->second.created) {
      oldest = it;
    }
  }
  if (count >= options_.max_per_user && oldest != sessions_.end()) {
    sessions_.erase(oldest);
  }
}

Result<IssuedToken> SessionStore::issue(std::string_view user, Scope scope) {
  std::array<unsigned char, kTokenBytes> random{};
  if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
    return std::unexpected(Error::make(ErrCode::internal, "RAND_bytes failed"));
  }
  IssuedToken out{.token = std::string{prefix(scope)} + to_hex(random), .expires_at = {}};
  CFD_TRY(digest, Sha256::of(out.token));

  const std::scoped_lock lock(mutex_);
  const Timestamp now = clock_->now();
  // Logins are rare next to other requests, so cleaning up here keeps the
  // table small without a timer.
  drop_expired(now);
  drop_oldest_of(user, scope);
  out.expires_at = now + (scope == Scope::admin ? options_.admin_ttl : options_.ttl);
  sessions_.insert_or_assign(digest, Session{.user = std::string{user},
                                             .scope = scope,
                                             .created = now,
                                             .last_used = now,
                                             .expires_at = out.expires_at});
  return out;
}

Result<std::string> SessionStore::verify(std::string_view token, Scope scope) {
  if (!token.starts_with(prefix(scope))) {
    return denied("invalid token");
  }
  // Looked up by digest: the map compares hashes, never the secret itself.
  CFD_TRY(digest, Sha256::of(token));

  const std::scoped_lock lock(mutex_);
  const auto it = sessions_.find(digest);
  if (it == sessions_.end() || it->second.scope != scope) {
    return denied("invalid or expired token");
  }
  const Timestamp now = clock_->now();
  if (expired(it->second, now)) {
    sessions_.erase(it);
    return denied("invalid or expired token");
  }
  it->second.last_used = now;
  return it->second.user;
}

void SessionStore::revoke(std::string_view token) {
  auto digest = Sha256::of(token);
  if (!digest) {
    return;
  }
  const std::scoped_lock lock(mutex_);
  sessions_.erase(*digest);
}

std::size_t SessionStore::revoke_user(std::string_view user) {
  const std::scoped_lock lock(mutex_);
  return std::erase_if(sessions_, [&](const auto& kv) { return kv.second.user == user; });
}

std::vector<SessionInfo> SessionStore::list() {
  const std::scoped_lock lock(mutex_);
  drop_expired(clock_->now());
  std::vector<SessionInfo> out;
  out.reserve(sessions_.size());
  for (const auto& [digest, s] : sessions_) {
    out.push_back({.user = s.user,
                   .scope = s.scope,
                   .created = s.created,
                   .last_used = s.last_used,
                   .expires_at = s.expires_at});
  }
  std::ranges::sort(out, {}, &SessionInfo::created);
  return out;
}

std::size_t SessionStore::size() const {
  const std::scoped_lock lock(mutex_);
  return sessions_.size();
}

}  // namespace confide::auth
