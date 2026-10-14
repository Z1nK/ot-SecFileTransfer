#include "config/parser.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <unordered_set>
#include <utility>

namespace confide::config::detail {

namespace fs = std::filesystem;
using common::ErrCode;
using common::Error;

namespace {

std::unexpected<Error> invalid(std::string detail) {
  return std::unexpected(Error::make(ErrCode::validation, std::move(detail)));
}

// Rules shared by `[[user]]` blocks and users_file lines.
Result<void> add_user(UserCfg user, std::string_view where, std::unordered_set<std::string>& seen,
                      std::vector<UserCfg>& users) {
  if (!is_valid_name(user.name)) {
    return invalid(
        std::format("{}.name: '{}' must be 1-64 chars of [A-Za-z0-9._-]", where, user.name));
  }
  if (user.password_hash.empty()) {
    return invalid(std::format("{}.password_hash is required", where));
  }
  if (!seen.insert(user.name).second) {
    return invalid(std::format("{}.name: duplicate user '{}'", where, user.name));
  }
  users.push_back(std::move(user));
  return {};
}

// Rules shared by `[[group]]` blocks and groups_file lines. `users` are the
// names already loaded: members must be among them, the group must not be.
Result<void> add_group(GroupCfg group, std::string_view where,
                       const std::unordered_set<std::string>& users,
                       std::unordered_set<std::string>& seen, std::vector<GroupCfg>& groups) {
  if (!is_valid_name(group.name)) {
    return invalid(
        std::format("{}.name: '{}' must be 1-64 chars of [A-Za-z0-9._-]", where, group.name));
  }
  if (users.contains(group.name)) {
    return invalid(std::format("{}.name: '{}' is already a user name", where, group.name));
  }
  if (!seen.insert(group.name).second) {
    return invalid(std::format("{}.name: duplicate group '{}'", where, group.name));
  }
  std::unordered_set<std::string_view> members;
  for (const auto& m : group.members) {
    if (!users.contains(m)) {
      return invalid(std::format("{}.members: '{}' is not a configured user", where, m));
    }
    if (!members.insert(m).second) {
      return invalid(std::format("{}.members: '{}' is listed twice", where, m));
    }
  }
  groups.push_back(std::move(group));
  return {};
}

// Calls `on_line(text, where)` for every line of a passwd-like file that is
// not blank or a '#' comment, with surrounding blanks removed. `key` names
// the setting in io errors; `where` is `path:lineno`.
template <class OnLine>
Result<void> for_each_line(const fs::path& path, std::string_view key, OnLine&& on_line) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::unexpected(
        Error::make(ErrCode::io, std::format("{}: cannot open '{}'", key, path.string())));
  }
  std::string line;
  std::size_t lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
      line.pop_back();
    }
    const auto start = line.find_first_not_of(" \t");
    if (start == std::string::npos || line[start] == '#') {
      continue;
    }
    CFD_TRYV(
        on_line(std::string_view(line).substr(start), std::format("{}:{}", path.string(), lineno)));
  }
  if (in.bad()) {
    return std::unexpected(
        Error::make(ErrCode::io, std::format("{}: read error in '{}'", key, path.string())));
  }
  return {};
}

// "a, b ,c" -> {"a", "b", "c"}; "" -> {}. An empty item stays empty, so
// add_group reports it as an unknown member.
std::vector<std::string> split_members(std::string_view s) {
  std::vector<std::string> out;
  if (s.find_first_not_of(" \t") == std::string_view::npos) {
    return out;
  }
  while (true) {
    const auto comma = s.find(',');
    auto item = s.substr(0, comma);
    const auto b = item.find_first_not_of(" \t");
    const auto e = item.find_last_not_of(" \t");
    out.emplace_back(b == std::string_view::npos ? std::string_view{} : item.substr(b, e - b + 1));
    if (comma == std::string_view::npos) {
      return out;
    }
    s.remove_prefix(comma + 1);
  }
}

}  // namespace

Result<Config> Parser::parse(std::istream& is, const std::string& source_name, fs::path base_dir) {
  auto parsed = toml::try_parse(is, source_name);
  if (parsed.is_err()) {
    std::string detail;
    for (const auto& e : parsed.unwrap_err()) {
      detail += toml::format_error(e);
    }
    return invalid(std::move(detail));
  }
  return Parser(parsed.unwrap(), std::move(base_dir)).run();
}

Result<const toml::value*> Parser::section(const toml::value& tbl, const std::string& key) {
  if (!tbl.contains(key)) {
    return nullptr;
  }
  const auto& v = tbl.at(key);
  if (!v.is_table()) {
    return invalid(std::format("{}: expected a table", key));
  }
  return &v;
}

Result<void> Parser::get(const toml::value& tbl, const std::string& key, std::string_view where,
                         std::string& out) {
  if (!tbl.contains(key)) {
    return {};
  }
  const auto& v = tbl.at(key);
  if (!v.is_string()) {
    return invalid(std::format("{}.{}: expected a string", where, key));
  }
  out = v.as_string();
  return {};
}

Result<void> Parser::get(const toml::value& tbl, const std::string& key, std::string_view where,
                         bool& out) {
  if (!tbl.contains(key)) {
    return {};
  }
  const auto& v = tbl.at(key);
  if (!v.is_boolean()) {
    return invalid(std::format("{}.{}: expected true or false", where, key));
  }
  out = v.as_boolean();
  return {};
}

template <class T>
  requires std::is_unsigned_v<T>
Result<void> Parser::get(const toml::value& tbl, const std::string& key, std::string_view where,
                         T& out) {
  if (!tbl.contains(key)) {
    return {};
  }
  const auto& v = tbl.at(key);
  if (!v.is_integer()) {
    return invalid(std::format("{}.{}: expected an integer", where, key));
  }
  const auto n = v.as_integer();
  if (n < 0 || static_cast<uint64_t>(n) > std::numeric_limits<T>::max()) {
    return invalid(std::format("{}.{}: {} is out of range [0, {}]", where, key, n,
                               std::numeric_limits<T>::max()));
  }
  out = static_cast<T>(n);
  return {};
}

// Relative paths are resolved against the config file's directory, so
// the server behaves the same whatever its working directory is.
Result<void> Parser::get(const toml::value& tbl, const std::string& key, std::string_view where,
                         fs::path& out) const {
  std::string s;
  if (auto r = get(tbl, key, where, s); !r) {
    return r;
  }
  if (!s.empty()) {
    fs::path p(s);
    out = (p.is_relative() && !base_dir_.empty()) ? (base_dir_ / p).lexically_normal() : p;
  }
  return {};
}

void Parser::warn_unknown(const toml::value& tbl, std::string_view where,
                          std::initializer_list<std::string_view> known) {
  for (const auto& [k, _] : tbl.as_table()) {
    if (std::ranges::find(known, k) == known.end()) {
      warnings_.push_back(std::format("{}{}: unknown key, ignored", where, k));
    }
  }
}

Result<void> Parser::parse_storage(StorageCfg& out) {
  CFD_TRY(tbl, section(root_, "storage"));
  if (tbl == nullptr) {
    return invalid("[storage] section is required");
  }
  warn_unknown(*tbl, "storage.", {"root"});
  CFD_TRYV(get(*tbl, "root", "storage", out.root));
  if (out.root.empty()) {
    return invalid("storage.root is required");
  }
  return {};
}

Result<void> Parser::parse_rest(RestCfg& out) {
  CFD_TRY(tbl, section(root_, "rest"));
  if (tbl == nullptr) {
    return {};
  }
  warn_unknown(*tbl, "rest.", {"bind", "port", "threads", "request_timeout_ms"});
  CFD_TRYV(get(*tbl, "bind", "rest", out.bind));
  CFD_TRYV(get(*tbl, "port", "rest", out.port));
  CFD_TRYV(get(*tbl, "threads", "rest", out.threads));
  CFD_TRYV(get(*tbl, "request_timeout_ms", "rest", out.request_timeout_ms));
  if (out.bind.empty()) {
    return invalid("rest.bind must not be empty");
  }
  if (out.port == 0) {
    return invalid("rest.port must be in [1, 65535]");
  }
  if (out.threads > 1024) {
    return invalid("rest.threads must be <= 1024 (0 = number of cores)");
  }
  if (out.request_timeout_ms == 0) {
    return invalid("rest.request_timeout_ms must be > 0");
  }
  return {};
}

Result<void> Parser::parse_tls(TlsCfg& out) {
  CFD_TRY(tbl, section(root_, "tls"));
  if (tbl != nullptr) {
    warn_unknown(*tbl, "tls.", {"enabled", "cert", "key", "ca"});
    CFD_TRYV(get(*tbl, "enabled", "tls", out.enabled));
    CFD_TRYV(get(*tbl, "cert", "tls", out.cert_path));
    CFD_TRYV(get(*tbl, "key", "tls", out.key_path));
    CFD_TRYV(get(*tbl, "ca", "tls", out.ca_path));
  }
  if (!out.enabled) {
    warnings_.emplace_back("tls.enabled = false: passwords and files travel in cleartext");
    return {};
  }
  if (out.cert_path.empty() || out.key_path.empty()) {
    return invalid("tls.cert and tls.key are required when TLS is enabled (the default)");
  }
  return {};
}

Result<void> Parser::parse_auth(AuthCfg& out) {
  CFD_TRY(tbl, section(root_, "auth"));
  if (tbl != nullptr) {
    warn_unknown(*tbl, "auth.",
                 {"provider", "token_ttl_s", "token_idle_s", "users_file", "groups_file", "ldap"});
    std::string provider;
    CFD_TRYV(get(*tbl, "provider", "auth", provider));
    if (provider == "ldap") {
      out.provider = AuthProvider::ldap;
    } else if (!provider.empty() && provider != "local") {
      return invalid(std::format("auth.provider: '{}' is not one of local, ldap", provider));
    }
    CFD_TRYV(get(*tbl, "token_ttl_s", "auth", out.token_ttl_s));
    CFD_TRYV(get(*tbl, "token_idle_s", "auth", out.token_idle_s));
    CFD_TRYV(get(*tbl, "users_file", "auth", out.users_file));
    CFD_TRYV(get(*tbl, "groups_file", "auth", out.groups_file));
    if (out.token_ttl_s == 0) {
      return invalid("auth.token_ttl_s must be > 0");
    }
    if (out.token_idle_s == 0) {
      return invalid("auth.token_idle_s must be > 0");
    }
  }

  // One user source only: the other one's settings are reported, not loaded.
  if (out.provider == AuthProvider::ldap) {
    CFD_TRYV(parse_ldap(tbl, out.ldap));
    if (!out.users_file.empty() || root_.contains("user")) {
      warnings_.emplace_back(
          "auth.users_file / [[user]]: ignored with auth.provider = \"ldap\"");
    }
    if (!out.groups_file.empty() || root_.contains("group")) {
      warnings_.emplace_back(
          "auth.groups_file / [[group]]: ignored with auth.provider = \"ldap\"");
    }
    return {};
  }
  if (tbl != nullptr && tbl->contains("ldap")) {
    warnings_.emplace_back("[auth.ldap]: ignored with auth.provider = \"local\"");
  }

  std::unordered_set<std::string> seen;
  if (!out.users_file.empty()) {
    CFD_TRYV(parse_users_file(out.users_file, seen, out));
  }

  if (root_.contains("user")) {
    CFD_TRYV(parse_inline_users(seen, out));
  }
  if (out.users.empty()) {
    warnings_.emplace_back("no users configured (auth.users_file or [[user]]): nobody can log in");
  }

  // Groups after all users: members are checked against them.
  std::unordered_set<std::string> seen_groups;
  if (!out.groups_file.empty()) {
    CFD_TRYV(parse_groups_file(out.groups_file, seen, seen_groups, out));
  }
  if (root_.contains("group")) {
    CFD_TRYV(parse_inline_groups(seen, seen_groups, out));
  }
  return {};
}

// `[auth.ldap]`, required with provider = "ldap". `auth` is the [auth]
// table, nullptr if there is none.
Result<void> Parser::parse_ldap(const toml::value* auth, LdapCfg& out) {
  const toml::value* tbl = nullptr;
  if (auth != nullptr) {
    CFD_TRY(t, section(*auth, "ldap"));
    tbl = t;
  }
  if (tbl == nullptr) {
    return invalid("[auth.ldap] section is required with auth.provider = \"ldap\"");
  }
  constexpr std::string_view where = "auth.ldap";
  warn_unknown(*tbl, "auth.ldap.",
               {"preset", "uri", "start_tls", "tls_verify", "ca_file", "timeout_s", "bind_dn",
                "bind_password", "bind_password_file", "user_base_dn", "user_filter",
                "username_attribute", "group_base_dn", "group_filter", "member_of_filter",
                "group_name_attribute", "cache_ttl_s"});
  CFD_TRYV(get(*tbl, "preset", where, out.preset));
  CFD_TRYV(get(*tbl, "uri", where, out.uri));
  CFD_TRYV(get(*tbl, "start_tls", where, out.start_tls));
  CFD_TRYV(get(*tbl, "tls_verify", where, out.tls_verify));
  CFD_TRYV(get(*tbl, "ca_file", where, out.ca_file));
  CFD_TRYV(get(*tbl, "timeout_s", where, out.timeout_s));
  CFD_TRYV(get(*tbl, "bind_dn", where, out.bind_dn));
  CFD_TRYV(get(*tbl, "bind_password", where, out.bind_password));
  CFD_TRYV(get(*tbl, "bind_password_file", where, out.bind_password_file));
  CFD_TRYV(get(*tbl, "user_base_dn", where, out.user_base_dn));
  CFD_TRYV(get(*tbl, "user_filter", where, out.user_filter));
  CFD_TRYV(get(*tbl, "username_attribute", where, out.username_attribute));
  CFD_TRYV(get(*tbl, "group_base_dn", where, out.group_base_dn));
  CFD_TRYV(get(*tbl, "group_filter", where, out.group_filter));
  CFD_TRYV(get(*tbl, "member_of_filter", where, out.member_of_filter));
  CFD_TRYV(get(*tbl, "group_name_attribute", where, out.group_name_attribute));
  CFD_TRYV(get(*tbl, "cache_ttl_s", where, out.cache_ttl_s));

  if (out.preset != "openldap" && out.preset != "active_directory") {
    return invalid(std::format("auth.ldap.preset: '{}' is not one of openldap, active_directory",
                               out.preset));
  }
  if (out.uri.empty()) {
    return invalid("auth.ldap.uri is required");
  }
  if (out.user_base_dn.empty()) {
    return invalid("auth.ldap.user_base_dn is required");
  }
  if (out.timeout_s == 0) {
    return invalid("auth.ldap.timeout_s must be > 0");
  }
  if (!out.bind_password.empty() && !out.bind_password_file.empty()) {
    return invalid("auth.ldap: set bind_password or bind_password_file, not both");
  }
  if (!out.bind_password_file.empty()) {
    CFD_TRYV(read_secret(out.bind_password_file, "auth.ldap.bind_password_file",
                         out.bind_password));
  }
  if (!out.tls_verify) {
    warnings_.emplace_back("auth.ldap.tls_verify = false: any LDAP server certificate is accepted");
  }
  if (out.uri.starts_with("ldap://") && !out.start_tls) {
    warnings_.emplace_back(
        "auth.ldap: ldap:// without start_tls: user passwords travel to LDAP in cleartext");
  }
  return {};
}

// Whole file, trailing newlines and blanks removed. Same chmod warning as
// users_file: it holds a secret.
Result<void> Parser::read_secret(const fs::path& path, std::string_view key, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::unexpected(
        Error::make(ErrCode::io, std::format("{}: cannot open '{}'", key, path.string())));
  }
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  if (in.bad()) {
    return std::unexpected(
        Error::make(ErrCode::io, std::format("{}: read error in '{}'", key, path.string())));
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '
                          || out.back() == '\t')) {
    out.pop_back();
  }
  if (out.empty()) {
    return invalid(std::format("{}: '{}' is empty", key, path.string()));
  }

  std::error_code ec;
  const auto perms = fs::status(path, ec).permissions();
  if (!ec && (perms & (fs::perms::group_read | fs::perms::others_read)) != fs::perms::none) {
    warnings_.push_back(
        std::format("{}: '{}' is readable by other users (chmod 600)", key, path.string()));
  }
  return {};
}

Result<void> Parser::parse_inline_users(std::unordered_set<std::string>& seen, AuthCfg& out) {
  const auto& users = root_.at("user");
  if (!users.is_array()) {
    return invalid("user: expected an array of tables ([[user]])");
  }
  std::size_t i = 0;
  for (const auto& u : users.as_array()) {
    const std::string where = std::format("user[{}]", i++);
    if (!u.is_table()) {
      return invalid(std::format("{}: expected a table", where));
    }
    // NFR-7: refuse plaintext outright rather than hash it for the operator
    // -- a plaintext secret sitting in the config file is the actual problem.
    if (u.contains("password")) {
      return invalid(
          std::format("{}.password: plaintext passwords are not allowed, "
                      "use password_hash",
                      where));
    }
    warn_unknown(u, where + ".", {"name", "password_hash"});
    UserCfg user;
    CFD_TRYV(get(u, "name", where, user.name));
    CFD_TRYV(get(u, "password_hash", where, user.password_hash));
    CFD_TRYV(add_user(std::move(user), where, seen, out.users));
  }
  return {};
}

Result<void> Parser::parse_inline_groups(const std::unordered_set<std::string>& users,
                                         std::unordered_set<std::string>& seen, AuthCfg& out) {
  const auto& groups = root_.at("group");
  if (!groups.is_array()) {
    return invalid("group: expected an array of tables ([[group]])");
  }
  std::size_t i = 0;
  for (const auto& g : groups.as_array()) {
    const std::string where = std::format("group[{}]", i++);
    if (!g.is_table()) {
      return invalid(std::format("{}: expected a table", where));
    }
    warn_unknown(g, where + ".", {"name", "members"});
    GroupCfg group;
    CFD_TRYV(get(g, "name", where, group.name));
    if (g.contains("members")) {
      const auto& members = g.at("members");
      if (!members.is_array()) {
        return invalid(std::format("{}.members: expected an array of strings", where));
      }
      for (const auto& m : members.as_array()) {
        if (!m.is_string()) {
          return invalid(std::format("{}.members: expected an array of strings", where));
        }
        group.members.push_back(m.as_string());
      }
    }
    CFD_TRYV(add_group(std::move(group), where, users, seen, out.groups));
  }
  return {};
}

// Same format idea as /etc/passwd: `name:password_hash`, one per line.
// '#' starts a comment line. A bad line is an error, never skipped.
Result<void> Parser::parse_users_file(const fs::path& path, std::unordered_set<std::string>& seen,
                                      AuthCfg& out) {
  CFD_TRYV(for_each_line(path, "auth.users_file",
                         [&](std::string_view line, const std::string& where) -> Result<void> {
                           const auto colon = line.find(':');
                           if (colon == std::string_view::npos) {
                             return invalid(
                                 std::format("{}: expected 'name:password_hash'", where));
                           }
                           UserCfg user{.name = std::string(line.substr(0, colon)),
                                        .password_hash = std::string(line.substr(colon + 1))};
                           return add_user(std::move(user), where, seen, out.users);
                         }));

  std::error_code ec;
  const auto perms = fs::status(path, ec).permissions();
  if (!ec && (perms & (fs::perms::group_read | fs::perms::others_read)) != fs::perms::none) {
    warnings_.push_back(std::format(
        "auth.users_file: '{}' is readable by other users (chmod 600)", path.string()));
  }
  return {};
}

// Like /etc/group without the password and GID fields:
// `name:member,member,...`, one group per line. No secrets, so no chmod check.
Result<void> Parser::parse_groups_file(const fs::path& path,
                                       const std::unordered_set<std::string>& users,
                                       std::unordered_set<std::string>& seen, AuthCfg& out) {
  return for_each_line(
      path, "auth.groups_file",
      [&](std::string_view line, const std::string& where) -> Result<void> {
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
          return invalid(std::format("{}: expected 'name:member,member,...'", where));
        }
        GroupCfg group{.name = std::string(line.substr(0, colon)),
                       .members = split_members(line.substr(colon + 1))};
        return add_group(std::move(group), where, users, seen, out.groups);
      });
}

Result<void> Parser::parse_log(LogCfg& out, const StorageCfg& storage) {
  CFD_TRY(tbl, section(root_, "log"));
  if (tbl != nullptr) {
    warn_unknown(*tbl, "log.", {"level", "dir", "rotate_bytes"});
    std::string level;
    CFD_TRYV(get(*tbl, "level", "log", level));
    if (!level.empty()) {
      using logging::LogLevel;
      bool found = false;
      for (auto l :
           {LogLevel::trace, LogLevel::debug, LogLevel::info, LogLevel::warn, LogLevel::error}) {
        if (logging::to_string(l) == level) {
          out.level = l;
          found = true;
        }
      }
      if (!found) {
        return invalid(
            std::format("log.level: '{}' is not one of trace, debug, info, warn, error", level));
      }
    }
    CFD_TRYV(get(*tbl, "dir", "log", out.dir));
    CFD_TRYV(get(*tbl, "rotate_bytes", "log", out.rotate_bytes));  // 0 = never rotate
  }
  if (out.dir.empty()) {
    out.dir = storage.root / "logs";
  }
  return {};
}

Result<void> Parser::parse_retention(RetentionCfg& out) {
  CFD_TRY(tbl, section(root_, "retention"));
  if (tbl == nullptr) {
    return {};
  }
  warn_unknown(*tbl, "retention.", {"default_days", "max_days", "sweep_interval_s"});
  CFD_TRYV(get(*tbl, "default_days", "retention", out.default_days));
  CFD_TRYV(get(*tbl, "max_days", "retention", out.max_days));
  CFD_TRYV(get(*tbl, "sweep_interval_s", "retention", out.sweep_interval_s));
  if (out.default_days == 0) {
    return invalid("retention.default_days must be > 0");
  }
  if (out.max_days < out.default_days) {
    return invalid(std::format("retention.max_days ({}) must be >= default_days ({})", out.max_days,
                               out.default_days));
  }
  if (out.sweep_interval_s == 0) {
    return invalid("retention.sweep_interval_s must be > 0");
  }
  return {};
}

Result<void> Parser::parse_peers(std::vector<PeerCfg>& out, const std::string& instance_name) {
  if (!root_.contains("peer")) {
    return {};
  }
  const auto& peers = root_.at("peer");
  if (!peers.is_array()) {
    return invalid("peer: expected an array of tables ([[peer]])");
  }
  std::unordered_set<std::string> seen;
  std::size_t i = 0;
  for (const auto& p : peers.as_array()) {
    const std::string where = std::format("peer[{}]", i++);
    if (!p.is_table()) {
      return invalid(std::format("{}: expected a table", where));
    }
    warn_unknown(p, where + ".", {"name", "host", "port", "token", "accept_token"});
    PeerCfg peer;
    CFD_TRYV(get(p, "name", where, peer.name));
    CFD_TRYV(get(p, "host", where, peer.host));
    CFD_TRYV(get(p, "port", where, peer.port));
    CFD_TRYV(get(p, "token", where, peer.token));
    CFD_TRYV(get(p, "accept_token", where, peer.accept_token));
    if (!is_valid_name(peer.name)) {
      return invalid(
          std::format("{}.name: '{}' must be 1-64 chars of [A-Za-z0-9._-]", where, peer.name));
    }
    if (peer.name == instance_name) {
      return invalid(std::format("{}.name: '{}' is this instance's own name", where, peer.name));
    }
    if (!seen.insert(peer.name).second) {
      return invalid(std::format("{}.name: duplicate peer '{}'", where, peer.name));
    }
    if (peer.host.empty()) {
      return invalid(std::format("{}.host is required", where));
    }
    if (peer.port == 0) {
      return invalid(std::format("{}.port must be in [1, 65535]", where));
    }
    if (peer.token.empty() || peer.accept_token.empty()) {
      return invalid(std::format("{}: token and accept_token are both required", where));
    }
    if (peer.token == peer.accept_token) {
      return invalid(std::format("{}: token and accept_token must differ", where));
    }
    out.push_back(std::move(peer));
  }
  return {};
}

Result<Config> Parser::run() {
  if (!root_.is_table()) {
    return invalid("config root must be a table");
  }
  warn_unknown(root_, "",
               {"instance_name", "storage", "rest", "tls", "auth", "user", "group", "log",
                "retention", "peer"});

  std::string instance_name;
  CFD_TRYV(get(root_, "instance_name", "config", instance_name));
  if (!is_valid_name(instance_name)) {
    return invalid(
        std::format("instance_name: '{}' must be 1-64 chars of [A-Za-z0-9._-]", instance_name));
  }

  StorageCfg storage;
  RestCfg rest;
  TlsCfg tls;
  AuthCfg auth;
  LogCfg log;
  RetentionCfg retention;
  std::vector<PeerCfg> peers;
  CFD_TRYV(parse_storage(storage));
  CFD_TRYV(parse_rest(rest));
  CFD_TRYV(parse_tls(tls));
  CFD_TRYV(parse_auth(auth));
  CFD_TRYV(parse_log(log, storage));
  CFD_TRYV(parse_retention(retention));
  CFD_TRYV(parse_peers(peers, instance_name));

  return Config(std::move(instance_name), std::move(storage), std::move(rest), std::move(tls),
                std::move(auth), std::move(log), retention, std::move(peers), std::move(warnings_));
}

}  // namespace confide::config::detail
