#include "auth/authenticator.hpp"

#include "common/base64.hpp"

#include <openssl/crypto.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <optional>

namespace confide::auth {

namespace {

using common::ErrCode;
using common::Error;
using common::base64_decode;
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

}  // namespace

Result<Authenticator> Authenticator::create(const config::Config& cfg,
                                            const IUserDirectory& directory,
                                            const common::IClock& clock,
                                            LoginLimiterOptions limits) {
  std::vector<Peer> peers;
  for (const auto& p : cfg.peers()) {
    if (p.accept_token.empty()) {
      continue;
    }
    CFD_TRY(digest, Sha256::of(p.accept_token));
    peers.push_back(Peer{.name = p.name, .token = digest});
  }
  auto sessions = std::make_unique<SessionStore>(
      clock, SessionOptions{.ttl = std::chrono::seconds{cfg.auth().token_ttl_s},
                            .idle = std::chrono::seconds{cfg.auth().token_idle_s}});
  return Authenticator(directory, std::move(peers), std::move(sessions),
                       std::make_unique<LoginLimiter>(clock, limits));
}

Result<std::string> Authenticator::authenticate_user(std::string_view authorization,
                                                     std::string_view source) const {
  if (const auto token = credentials(authorization, "Bearer")) {
    return sessions_->verify(*token);
  }
  return authenticate_password(authorization, source);
}

Result<std::string> Authenticator::authenticate_password(std::string_view authorization,
                                                         std::string_view source) const {
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

  // Checked before the directory, so a throttled guess never reaches LDAP.
  CFD_TRYV(limiter_->check(user, source));
  auto name = directory_->verify(user, password);
  if (name) {
    limiter_->succeeded(user, source);
  } else if (name.error().code == ErrCode::auth) {
    limiter_->failed(user, source);
  }
  return name;
}

Result<IssuedToken> Authenticator::issue_token(std::string_view user) const {
  return sessions_->issue(user);
}

void Authenticator::revoke_token(std::string_view authorization) const {
  if (const auto token = credentials(authorization, "Bearer")) {
    sessions_->revoke(*token);
  }
}

Result<std::string> Authenticator::authenticate_peer(std::string_view authorization) const {
  if (authorization.empty()) {
    return denied("missing Authorization header");
  }
  const auto token = credentials(authorization, "Bearer");
  if (!token || token->empty()) {
    return denied("expected Bearer peer token");
  }
  if (token->starts_with(SessionStore::kPrefix)) {
    return denied("user tokens are not accepted from peers");
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
