#pragma once

// Internal to the config library: only config.cpp and parser.cpp include
// this. Everyone else goes through ConfigLoader.

#include "config/config.hpp"

#include <toml.hpp>

#include <filesystem>
#include <initializer_list>
#include <istream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace confide::config::detail {

// Turns one TOML document into a validated Config. Every getter leaves the
// target untouched when the key is absent, so the struct defaults in
// config.hpp stay the single source of truth for default values.
class Parser {
public:
  // `source_name` shows up in syntax error messages. Relative paths in the
  // document are resolved against `base_dir`; empty = keep them as written.
  static Result<Config> parse(std::istream& is, const std::string& source_name,
                              std::filesystem::path base_dir);

private:
  Parser(const toml::value& root, std::filesystem::path base_dir)
      : root_(root), base_dir_(std::move(base_dir)) {}

  Result<Config> run();

  Result<void> parse_storage(StorageCfg& out);
  Result<void> parse_rest(RestCfg& out);
  Result<void> parse_tls(TlsCfg& out);
  Result<void> parse_auth(AuthCfg& out);
  // Reads `name:password_hash` lines from `path` into `out.users`.
  Result<void> parse_users_file(const std::filesystem::path& path,
                                std::unordered_set<std::string>& seen, AuthCfg& out);
  Result<void> parse_log(LogCfg& out, const StorageCfg& storage);
  Result<void> parse_retention(RetentionCfg& out);
  Result<void> parse_peers(std::vector<PeerCfg>& out, const std::string& instance_name);

  // Optional sub-table; nullptr when the section is absent.
  static Result<const toml::value*> section(const toml::value& tbl, const std::string& key);

  // Typed getters. `where` is the section prefix used in error messages.
  static Result<void> get(const toml::value& tbl, const std::string& key, std::string_view where,
                          std::string& out);
  static Result<void> get(const toml::value& tbl, const std::string& key, std::string_view where,
                          bool& out);
  template <class T>
    requires std::is_unsigned_v<T>
  static Result<void> get(const toml::value& tbl, const std::string& key, std::string_view where,
                          T& out);
  Result<void> get(const toml::value& tbl, const std::string& key, std::string_view where,
                   std::filesystem::path& out) const;

  // Typos like `prot = 8080` would otherwise be silently ignored.
  void warn_unknown(const toml::value& tbl, std::string_view where,
                    std::initializer_list<std::string_view> known);

  const toml::value& root_;
  std::filesystem::path base_dir_;
  std::vector<std::string> warnings_;
};

}  // namespace confide::config::detail
