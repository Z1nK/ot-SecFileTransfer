#pragma once

#include "common/error.hpp"
#include "logging/tech_log.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace confide::config {

using common::Result;

// Names end up in routing (`bob@siteB`) and Basic auth (`user:pass`), so
// keep them to a conservative charset: no '@', ':', '/', spaces. Applies to
// instance, peer, user and group names, local or from LDAP.
inline bool is_valid_name(std::string_view s) {
  return !s.empty() && s.size() <= 64 && std::ranges::all_of(s, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'
           || c == '_' || c == '.';
  });
}

// Root of the on-disk layout: `<root>/tmp/` and `<root>/transactions/`
struct StorageCfg {
  std::filesystem::path root;
};

// HTTP API listener configuration. 
struct RestCfg {
  std::string bind = "0.0.0.0";
  uint16_t port = 8080;
  uint32_t threads = 0;  // 0 = hardware_concurrency
  uint32_t request_timeout_ms = 30'000;
};

// On by default. Same cert/key serve the API and outgoing forwards.
struct TlsCfg {
  bool enabled = true;
  std::filesystem::path cert_path;
  std::filesystem::path key_path;
  // CA bundle used to verify peer servers; empty = system default.
  std::filesystem::path ca_path;
};

// local user; only the password hash is ever stored.
struct UserCfg {
  std::string name;
  std::string password_hash;
};

// named set of local users; a transaction sent to the group reaches every
// member. Shares one namespace with users.
struct GroupCfg {
  std::string name;
  std::vector<std::string> members;
};

// Where users and groups come from: local files/blocks or an LDAP directory.
// Exactly one is used.
enum class AuthProvider : uint8_t { local, ldap };

// `[auth.ldap]`, used with `provider = "ldap"`. Plain data: the config
// library knows nothing about the LDAP client. Empty filters/attributes keep
// the preset's default.
struct LdapCfg {
  std::string preset = "openldap";  // or "active_directory"
  // ldap://host:389 or ldaps://host:636; several, space-separated = failover.
  std::string uri;
  bool start_tls = false;
  bool tls_verify = true;  // false = accept any certificate (tests only)
  std::filesystem::path ca_file{};
  uint32_t timeout_s = 5;
  // Service account for lookups; empty bind_dn = anonymous. The password
  // comes from `bind_password` or is read from `bind_password_file`.
  std::string bind_dn{};
  std::string bind_password{};
  std::filesystem::path bind_password_file{};
  std::string user_base_dn{};
  std::string user_filter{};  // with {username}
  std::string username_attribute{};
  // Empty group_base_dn = no groups.
  std::string group_base_dn{};
  std::string group_filter{};
  std::string member_of_filter{};  // with {dn} / {username}
  std::string group_name_attribute{};
  // How long a verified password and looked-up users/groups are reused
  // without asking the server again; 0 = always ask.
  uint32_t cache_ttl_s = 300;
};

struct AuthCfg {
  // users from `users_file` and from `[[user]]` blocks, merged.
  std::vector<UserCfg> users;
  // passwd-like file with one `name:password_hash` per line; empty = none.
  std::filesystem::path users_file{};
  // groups from `groups_file` and from `[[group]]` blocks, merged.
  std::vector<GroupCfg> groups{};
  // group-like file with one `name:member,member,...` per line; empty = none.
  std::filesystem::path groups_file{};
  uint32_t token_ttl_s = 3600; // will try to implement tokens
  AuthProvider provider = AuthProvider::local;
  // Only used with provider = ldap; users/groups above stay empty then.
  LdapCfg ldap{};
};

// `technical.log` and `business.log` are both written into `dir`.
struct LogCfg {
  confide::logging::LogLevel level = confide::logging::LogLevel::info;
  std::filesystem::path dir;
  uint64_t rotate_bytes = 64ULL * 1024 * 1024;
};

// per-transaction retention, bounded by `max_days`.
struct RetentionCfg {
  uint32_t default_days = 30;
  uint32_t max_days = 365;
  uint32_t sweep_interval_s = 3600;
};

// one server we can forward committed transactions to. `token` is
// the one WE present to `name`; `accept_token` is the one `name` must
// present to us -- never reuse one secret in both directions.
struct PeerCfg {
  std::string name;
  std::string host;
  uint16_t port = 8080;
  std::string token;
  std::string accept_token;
};

// Immutable typed view of a parsed TOML config file. Construct only via
// ConfigLoader, which guarantees every field has already been validated.
class Config {
public:
  Config(std::string instance_name, StorageCfg storage, RestCfg rest, TlsCfg tls, AuthCfg auth,
         LogCfg log, RetentionCfg retention, std::vector<PeerCfg> peers = {},
         std::vector<std::string> warnings = {})
      : instance_name_(std::move(instance_name))
      , storage_(std::move(storage))
      , rest_(std::move(rest))
      , tls_(std::move(tls))
      , auth_(std::move(auth))
      , log_(std::move(log))
      , retention_(retention)
      , peers_(std::move(peers))
      , warnings_(std::move(warnings)) {}

  const std::string& instance_name() const { return instance_name_; }
  const StorageCfg& storage() const { return storage_; }
  const RestCfg& rest() const { return rest_; }
  const TlsCfg& tls() const { return tls_; }
  const AuthCfg& auth() const { return auth_; }
  const LogCfg& log() const { return log_; }
  const RetentionCfg& retention() const { return retention_; }
  const std::vector<PeerCfg>& peers() const { return peers_; }

  // nullptr if no peer/user with that name is configured (FR-13).
  const PeerCfg* find_peer(std::string_view name) const {
    for (const auto& p : peers_) {
      if (p.name == name) {
        return &p;
      }
    }
    return nullptr;
  }
  const UserCfg* find_user(std::string_view name) const {
    for (const auto& u : auth_.users) {
      if (u.name == name) {
        return &u;
      }
    }
    return nullptr;
  }
  const GroupCfg* find_group(std::string_view name) const {
    for (const auto& g : auth_.groups) {
      if (g.name == name) {
        return &g;
      }
    }
    return nullptr;
  }
  // true if `user` is listed in group `group`; false for unknown groups.
  bool is_member(std::string_view user, std::string_view group) const {
    const auto* g = find_group(group);
    return g != nullptr && std::ranges::find(g->members, user) != g->members.end();
  }

  // Non-fatal notices collected while parsing -- the caller prints these;
  // ConfigLoader itself has no logger to print through.
  const std::vector<std::string>& warnings() const { return warnings_; }

private:
  std::string instance_name_;
  StorageCfg storage_;
  RestCfg rest_;
  TlsCfg tls_;
  AuthCfg auth_;
  LogCfg log_;
  RetentionCfg retention_;
  std::vector<PeerCfg> peers_;
  std::vector<std::string> warnings_;
};

class ConfigLoader {
public:
  static Result<Config> from_file(const std::filesystem::path& path);
  static Result<Config> from_string(std::string_view toml);
};

}  // namespace confide::config
