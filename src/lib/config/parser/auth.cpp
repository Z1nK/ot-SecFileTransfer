#include "config/parser.hpp"

#include <format>
#include <fstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace confide::config::detail {

namespace fs = std::filesystem;
using common::ErrCode;
using common::Error;

namespace {

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

}  // namespace confide::config::detail
