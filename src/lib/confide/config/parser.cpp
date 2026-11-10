#include "config/parser.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace confide::config::detail {

namespace fs = std::filesystem;

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
