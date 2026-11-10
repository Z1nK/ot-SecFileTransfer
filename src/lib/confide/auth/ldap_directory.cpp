#include "auth/ldap_directory.hpp"

#include "common/sha256.hpp"

#include <ldapclient/ldapAuth.hpp>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

// Built only with WITH_LDAP. Users and groups come from an LDAP directory
// through ldapclient; every call opens its own short-lived connection, so
// results are cached for `cache_ttl_s`. Basic credentials come with every
// request, and group membership is asked once per transaction in listings.

namespace confide::auth {

namespace {

using common::ErrCode;
using common::Error;
using common::Sha256;
using common::Sha256Digest;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kTag = "auth.ldap";
constexpr std::string_view kDenied = "invalid user name or password";
constexpr std::string_view kUnavailable = "user directory unavailable";
// Expired entries are dropped once a cache grows past this.
constexpr std::size_t kPruneAbove = 1024;

std::unexpected<Error> denied() {
  return std::unexpected(Error::make(ErrCode::auth, std::string(kDenied)));
}

std::unexpected<Error> unavailable() {
  return std::unexpected(Error::make(ErrCode::unavailable, std::string(kUnavailable)));
}

bool same(const Sha256Digest& a, const Sha256Digest& b) {
  return CRYPTO_memcmp(a.bytes().data(), b.bytes().data(), Sha256Digest::kSize) == 0;
}

// LDAP names are case-insensitive.
bool iequals(std::string_view a, std::string_view b) {
  return std::ranges::equal(a, b, [](char x, char y) {
    return std::tolower(static_cast<unsigned char>(x))
           == std::tolower(static_cast<unsigned char>(y));
  });
}

ldapauth::Config to_ldapauth(const config::LdapCfg& c) {
  auto out = c.preset == "active_directory" ? ldapauth::Config::activeDirectory()
                                            : ldapauth::Config{};
  out.uri = c.uri;
  out.startTls = c.start_tls;
  out.tlsVerify = c.tls_verify;
  out.tlsCaFile = c.ca_file.string();
  out.timeoutSeconds = static_cast<int>(c.timeout_s);
  out.bindDn = c.bind_dn;
  out.bindPassword = c.bind_password;
  out.userBaseDn = c.user_base_dn;
  out.groupBaseDn = c.group_base_dn;
  const auto set = [](std::string& field, const std::string& value) {
    if (!value.empty()) {
      field = value;
    }
  };
  set(out.userFilter, c.user_filter);
  set(out.usernameAttribute, c.username_attribute);
  set(out.groupFilter, c.group_filter);
  set(out.memberOfFilter, c.member_of_filter);
  set(out.groupNameAttribute, c.group_name_attribute);
  return out;
}

class LdapDirectory final : public IUserDirectory {
public:
  LdapDirectory(ldapauth::Config cfg, std::chrono::seconds ttl,
                std::array<std::byte, 16> salt, logging::TechLog* log)
      : ldap_(std::move(cfg)), ttl_(ttl), salt_(salt), log_(log) {}

  Result<std::string> verify(std::string_view user, std::string_view password) const override {
    if (!config::is_valid_name(user)) {
      return denied();
    }
    CFD_TRY(print, fingerprint(user, password));
    {
      const std::lock_guard lock(mu_);
      const auto it = binds_.find(user);
      if (it != binds_.end() && fresh(it->second.at) && same(it->second.print, print)) {
        return it->second.name;
      }
    }

    ldapauth::AuthResult r;
    try {
      r = ldap_.authenticate(std::string(user), std::string(password));
    } catch (const ldapauth::LdapError& e) {
      return failed("login", user, e);
    }
    if (!r) {
      // The server's reason (e.g. AD "data 775" = locked) is for the log
      // only; the client sees one message for every failure.
      if (log_ != nullptr) {
        log_->info(kTag, "login '{}' refused: {} ({})", user, ldapauth::toString(r.status),
                   r.message);
      }
      return denied();
    }

    const auto now = Clock::now();
    const std::lock_guard lock(mu_);
    prune(binds_, now);
    binds_.insert_or_assign(std::string(user),
                            Bind{.print = print, .name = r.user->username, .at = now});
    prune(users_, now);
    users_.insert_or_assign(std::string(user), UserEntry{.groups = r.user->groups, .at = now});
    return r.user->username;
  }

  Result<bool> has_user(std::string_view name) const override {
    CFD_TRY(groups, groups_of(name));
    return groups.has_value();
  }

  Result<bool> has_group(std::string_view name) const override {
    if (!config::is_valid_name(name)) {
      return false;
    }
    {
      const std::lock_guard lock(mu_);
      const auto it = groups_.find(name);
      if (it != groups_.end() && fresh(it->second.at)) {
        return it->second.exists;
      }
    }
    bool exists = false;
    try {
      exists = ldap_.findGroup(std::string(name)).has_value();
    } catch (const ldapauth::LdapError& e) {
      return failed("group lookup", name, e);
    }
    const auto now = Clock::now();
    const std::lock_guard lock(mu_);
    prune(groups_, now);
    groups_.insert_or_assign(std::string(name), GroupEntry{.exists = exists, .at = now});
    return exists;
  }

  bool is_member(std::string_view user, std::string_view group) const override {
    auto groups = groups_of(user);
    if (!groups || !groups->has_value()) {
      return false;
    }
    return std::ranges::any_of(**groups, [&](const auto& g) { return iequals(g, group); });
  }

private:
  struct Bind {
    Sha256Digest print;
    std::string name;  // canonical, from the directory
    Clock::time_point at;
  };
  struct UserEntry {
    std::optional<std::vector<std::string>> groups;  // nullopt = no such user
    Clock::time_point at;
  };
  struct GroupEntry {
    bool exists = false;
    Clock::time_point at;
  };

  bool fresh(Clock::time_point at) const { return Clock::now() - at < ttl_; }

  template <class Map>
  void prune(Map& m, Clock::time_point now) const {
    if (m.size() > kPruneAbove) {
      std::erase_if(m, [&](const auto& kv) { return now - kv.second.at >= ttl_; });
    }
  }

  // SHA-256(per-process salt || user || NUL || password): the cache key of
  // a password the directory accepted.
  Result<Sha256Digest> fingerprint(std::string_view user, std::string_view password) const {
    CFD_TRY(sha, Sha256::create());
    CFD_TRYV(sha.update(common::ByteSpan(salt_)));
    CFD_TRYV(sha.update(user));
    CFD_TRYV(sha.update(std::string_view("\0", 1)));
    CFD_TRYV(sha.update(password));
    return sha.finish();
  }

  // The user's group names; nullopt if there is no such user.
  Result<std::optional<std::vector<std::string>>> groups_of(std::string_view user) const {
    if (!config::is_valid_name(user)) {
      return std::nullopt;
    }
    {
      const std::lock_guard lock(mu_);
      const auto it = users_.find(user);
      if (it != users_.end() && fresh(it->second.at)) {
        return it->second.groups;
      }
    }
    std::optional<std::vector<std::string>> groups;
    try {
      if (auto u = ldap_.findUser(std::string(user))) {
        groups = std::move(u->groups);
      }
    } catch (const ldapauth::LdapError& e) {
      return failed("user lookup", user, e);
    }
    const auto now = Clock::now();
    const std::lock_guard lock(mu_);
    prune(users_, now);
    users_.insert_or_assign(std::string(user), UserEntry{.groups = groups, .at = now});
    return groups;
  }

  std::unexpected<Error> failed(std::string_view what, std::string_view name,
                                const ldapauth::LdapError& e) const {
    if (log_ != nullptr) {
      log_->error(kTag, "{} '{}': LDAP error {}: {}", what, name, e.code(), e.what());
    }
    return unavailable();
  }

  ldapauth::LdapAuth ldap_;
  std::chrono::seconds ttl_;
  std::array<std::byte, 16> salt_;
  logging::TechLog* log_;

  mutable std::mutex mu_;
  mutable std::map<std::string, Bind, std::less<>> binds_;
  mutable std::map<std::string, UserEntry, std::less<>> users_;
  mutable std::map<std::string, GroupEntry, std::less<>> groups_;
};

}  // namespace

Result<std::unique_ptr<IUserDirectory>> make_ldap_directory(const config::LdapCfg& cfg,
                                                            logging::TechLog* log) {
  std::array<std::byte, 16> salt{};
  if (RAND_bytes(reinterpret_cast<unsigned char*>(salt.data()), static_cast<int>(salt.size()))
      != 1) {
    return std::unexpected(Error::make(ErrCode::internal, "RAND_bytes failed"));
  }
  return std::make_unique<LdapDirectory>(to_ldapauth(cfg), std::chrono::seconds(cfg.cache_ttl_s),
                                         salt, log);
}

}  // namespace confide::auth
