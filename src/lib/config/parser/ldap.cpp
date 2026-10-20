#include "config/parser.hpp"

#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace confide::config::detail {

namespace fs = std::filesystem;
using common::ErrCode;
using common::Error;

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

}  // namespace confide::config::detail
