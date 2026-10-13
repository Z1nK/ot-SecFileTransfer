#include "auth/session_store.hpp"

#include <openssl/rand.h>

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

bool SessionStore::expired(const Session& s, Timestamp now) const {
  return common::is_expired(s.expires_at, now)
         || common::is_expired(s.last_used + options_.idle, now);
}

void SessionStore::drop_expired(Timestamp now) {
  std::erase_if(sessions_, [&](const auto& kv) { return expired(kv.second, now); });
}

void SessionStore::drop_oldest_of(std::string_view user) {
  std::size_t count = 0;
  auto oldest = sessions_.end();
  for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
    if (it->second.user != user) {
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

Result<IssuedToken> SessionStore::issue(std::string_view user) {
  std::array<unsigned char, kTokenBytes> random{};
  if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
    return std::unexpected(Error::make(ErrCode::internal, "RAND_bytes failed"));
  }
  IssuedToken out{.token = std::string{kPrefix} + to_hex(random), .expires_at = {}};
  CFD_TRY(digest, Sha256::of(out.token));

  const std::scoped_lock lock(mutex_);
  const Timestamp now = clock_->now();
  // Logins are rare next to other requests, so cleaning up here keeps the
  // table small without a timer.
  drop_expired(now);
  drop_oldest_of(user);
  out.expires_at = now + options_.ttl;
  sessions_.insert_or_assign(digest, Session{.user = std::string{user},
                                             .created = now,
                                             .last_used = now,
                                             .expires_at = out.expires_at});
  return out;
}

Result<std::string> SessionStore::verify(std::string_view token) {
  if (!token.starts_with(kPrefix)) {
    return denied("invalid token");
  }
  // Looked up by digest: the map compares hashes, never the secret itself.
  CFD_TRY(digest, Sha256::of(token));

  const std::scoped_lock lock(mutex_);
  const auto it = sessions_.find(digest);
  if (it == sessions_.end()) {
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

std::size_t SessionStore::size() const {
  const std::scoped_lock lock(mutex_);
  return sessions_.size();
}

}  // namespace confide::auth
