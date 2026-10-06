#include "auth/authenticator.hpp"

#include "auth/base64.hpp"

#include <openssl/crypto.h>

#include <algorithm>
#include <cctype>
#include <format>

namespace confide::auth {

namespace {

using common::ErrCode;
using common::Error;
using common::Sha256;
using common::Sha256Digest;

std::unexpected<Error> denied(std::string detail) {
  return std::unexpected(Error::make(ErrCode::auth, std::move(detail)));
}

bool iequals(std::string_view a, std::string_view b) {
  return std::ranges::equal(a, b, [](char x, char y) {
    return std::tolower(static_cast<unsigned char>(x))
           == std::tolower(static_cast<unsigned char>(y));
  });
}

// Credentials of "<scheme> <credentials>"; the scheme is case-insensitive
// (RFC 9110 §11.1). nullopt if the header uses another scheme.
std::optional<std::string_view> credentials(std::string_view header, std::string_view scheme) {
  const auto space = header.find(' ');
  if (space == std::string_view::npos || !iequals(header.substr(0, space), scheme)) {
    return std::nullopt;
  }
  auto rest = header.substr(space);
  const auto first = rest.find_first_not_of(' ');
  return first == std::string_view::npos ? std::string_view{} : rest.substr(first);
}

bool same(const Sha256Digest& a, const Sha256Digest& b) {
  return CRYPTO_memcmp(a.bytes().data(), b.bytes().data(), Sha256Digest::kSize) == 0;
}

// SHA-256(salt || password): the cache key of a verified password.
Result<Sha256Digest> fingerprint(const PasswordHash& hash, std::string_view password) {
  CFD_TRY(sha, Sha256::create());
  CFD_TRYV(sha.update(common::ByteSpan(hash.salt())));
  CFD_TRYV(sha.update(password));
  return sha.finish();
}

}  // namespace

Result<Authenticator> Authenticator::create(const config::Config& cfg) {
  std::map<std::string, PasswordHash, std::less<>> users;
  for (const auto& u : cfg.auth().users) {
    auto hash = PasswordHash::parse(u.password_hash);
    if (!hash) {
      return std::unexpected(
          Error::make(ErrCode::validation,
                      std::format("user '{}': password_hash: {}", u.name, hash.error().detail)));
    }
    users.emplace(u.name, std::move(*hash));
  }

  std::vector<Peer> peers;
  for (const auto& p : cfg.peers()) {
    if (p.accept_token.empty()) {
      continue;
    }
    CFD_TRY(digest, Sha256::of(p.accept_token));
    peers.push_back(Peer{.name = p.name, .token = digest});
  }

  std::optional<PasswordHash> dummy;
  if (!users.empty()) {
    dummy = users.begin()->second;
  }
  return Authenticator(std::move(users), std::move(peers), std::move(dummy));
}

Result<std::string> Authenticator::authenticate_user(std::string_view authorization) const {
  if (authorization.empty()) {
    return denied("missing Authorization header");
  }
  const auto encoded = credentials(authorization, "Basic");
  if (!encoded) {
    return denied("expected Basic credentials");
  }
  auto decoded = base64_decode(*encoded);
  if (!decoded) {
    return denied("malformed Basic credentials");
  }
  const std::string_view pair(reinterpret_cast<const char*>(decoded->data()), decoded->size());
  const auto colon = pair.find(':');
  if (colon == std::string_view::npos) {
    return denied("malformed Basic credentials");
  }
  const auto user = pair.substr(0, colon);
  const auto password = pair.substr(colon + 1);

  const auto it = users_.find(user);
  if (it == users_.end()) {
    if (dummy_) {
      (void)dummy_->verify(password);
    }
    return denied("invalid user name or password");
  }
  if (!check_password(user, it->second, password)) {
    return denied("invalid user name or password");
  }
  return std::string(user);
}

bool Authenticator::check_password(std::string_view user, const PasswordHash& hash,
                                   std::string_view password) const {
  auto print = fingerprint(hash, password);
  if (print) {
    const std::lock_guard lock(cache_->mu);
    const auto it = cache_->by_user.find(user);
    if (it != cache_->by_user.end() && same(it->second, *print)) {
      return true;
    }
  }
  if (!hash.verify(password)) {
    return false;
  }
  if (print) {
    const std::lock_guard lock(cache_->mu);
    cache_->by_user.insert_or_assign(std::string(user), *print);
  }
  return true;
}

Result<std::string> Authenticator::authenticate_peer(std::string_view authorization) const {
  if (authorization.empty()) {
    return denied("missing Authorization header");
  }
  const auto token = credentials(authorization, "Bearer");
  if (!token || token->empty()) {
    return denied("expected Bearer peer token");
  }
  CFD_TRY(digest, Sha256::of(*token));

  // No early exit: the time doesn't tell how many peers were tried.
  const Peer* match = nullptr;
  for (const auto& p : peers_) {
    if (same(p.token, digest)) {
      match = &p;
    }
  }
  if (match == nullptr) {
    return denied("invalid peer token");
  }
  return match->name;
}

}  // namespace confide::auth
