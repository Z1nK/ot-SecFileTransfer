#include "auth/local_directory.hpp"

#include <openssl/crypto.h>

#include <algorithm>
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

Result<std::unique_ptr<LocalDirectory>> LocalDirectory::create(const config::Config& cfg) {
  Users users;
  for (const auto& u : cfg.auth().users) {
    auto hash = PasswordHash::parse(u.password_hash);
    if (!hash) {
      return std::unexpected(
          Error::make(ErrCode::validation,
                      std::format("user '{}': password_hash: {}", u.name, hash.error().detail)));
    }
    users.emplace(u.name, std::move(*hash));
  }

  Groups groups;
  for (const auto& g : cfg.auth().groups) {
    groups.emplace(g.name, g.members);
  }

  std::optional<PasswordHash> dummy;
  if (!users.empty()) {
    dummy = users.begin()->second;
  }
  return std::unique_ptr<LocalDirectory>(
      new LocalDirectory(std::move(users), std::move(groups), std::move(dummy)));
}

Result<std::string> LocalDirectory::verify(std::string_view user,
                                           std::string_view password) const {
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

bool LocalDirectory::check_password(std::string_view user, const PasswordHash& hash,
                                    std::string_view password) const {
  auto print = fingerprint(hash, password);
  if (print) {
    const std::lock_guard lock(mu_);
    const auto it = verified_.find(user);
    if (it != verified_.end() && same(it->second, *print)) {
      return true;
    }
  }
  if (!hash.verify(password)) {
    return false;
  }
  if (print) {
    const std::lock_guard lock(mu_);
    verified_.insert_or_assign(std::string(user), *print);
  }
  return true;
}

Result<bool> LocalDirectory::has_user(std::string_view name) const {
  return users_.contains(name);
}

Result<bool> LocalDirectory::has_group(std::string_view name) const {
  return groups_.contains(name);
}

bool LocalDirectory::is_member(std::string_view user, std::string_view group) const {
  const auto it = groups_.find(group);
  return it != groups_.end() && std::ranges::find(it->second, user) != it->second.end();
}

}  // namespace confide::auth
