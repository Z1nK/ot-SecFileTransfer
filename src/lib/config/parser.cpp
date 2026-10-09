#include "config/parser.hpp"

#include <algorithm>
#include <format>
#include <fstream>
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

// Names end up in routing (`bob@siteB`) and Basic auth (`user:pass`), so
// keep them to a conservative charset: no '@', ':', '/', spaces.
bool is_valid_name(std::string_view s) {
  return !s.empty() && s.size() <= 64 && std::ranges::all_of(s, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'
           || c == '_' || c == '.';
  });
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
    warn_unknown(*tbl, "auth.", {"token_ttl_s", "users_file"});
    CFD_TRYV(get(*tbl, "token_ttl_s", "auth", out.token_ttl_s));
    CFD_TRYV(get(*tbl, "users_file", "auth", out.users_file));
    if (out.token_ttl_s == 0) {
      return invalid("auth.token_ttl_s must be > 0");
    }
  }

  std::unordered_set<std::string> seen;
  if (!out.users_file.empty()) {
    CFD_TRYV(parse_users_file(out.users_file, seen, out));
  }

  if (!root_.contains("user")) {
    if (out.users.empty()) {
      warnings_.emplace_back("no users configured (auth.users_file or [[user]]): nobody can log in");
    }
    return {};
  }
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

// Same format idea as /etc/passwd: `name:password_hash`, one per line.
// '#' starts a comment line. A bad line is an error, never skipped.
Result<void> Parser::parse_users_file(const fs::path& path, std::unordered_set<std::string>& seen,
                                      AuthCfg& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::unexpected(Error::make(
        ErrCode::io, std::format("auth.users_file: cannot open '{}'", path.string())));
  }

  std::error_code ec;
  const auto perms = fs::status(path, ec).permissions();
  if (!ec && (perms & (fs::perms::group_read | fs::perms::others_read)) != fs::perms::none) {
    warnings_.push_back(std::format(
        "auth.users_file: '{}' is readable by other users (chmod 600)", path.string()));
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
    const std::string where = std::format("{}:{}", path.string(), lineno);
    const auto colon = line.find(':', start);
    if (colon == std::string::npos) {
      return invalid(std::format("{}: expected 'name:password_hash'", where));
    }
    UserCfg user{.name = line.substr(start, colon - start),
                 .password_hash = line.substr(colon + 1)};
    CFD_TRYV(add_user(std::move(user), where, seen, out.users));
  }
  if (in.bad()) {
    return std::unexpected(Error::make(
        ErrCode::io, std::format("auth.users_file: read error in '{}'", path.string())));
  }
  return {};
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
  warn_unknown(
      root_, "",
      {"instance_name", "storage", "rest", "tls", "auth", "user", "log", "retention", "peer"});

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
