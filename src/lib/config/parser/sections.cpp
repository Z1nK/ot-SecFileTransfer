#include "config/parser.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace confide::config::detail {

namespace fs = std::filesystem;

namespace {

// Why `secret` can't be a peer token, or "" if it can. Never names the secret.
std::string token_problem(std::string_view secret) {
  if (secret.size() < kMinPeerTokenLength) {
    return std::format("must be at least {} characters (e.g. openssl rand -hex 32)",
                       kMinPeerTokenLength);
  }
  // Sent as `Authorization: Bearer <token>`: no spaces or control characters.
  if (!std::ranges::all_of(secret, [](char c) { return c > ' ' && c < 0x7F; })) {
    return "must be visible ASCII characters only (no spaces)";
  }
  if (secret.starts_with("ftu_") || secret.starts_with("fta_")) {
    return "must not start with ftu_ or fta_ (the prefixes of user and admin tokens)";
  }
  return {};
}

}  // namespace

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
  warn_unknown(*tbl, "rest.", {"bind", "port", "threads", "blocking_threads", "request_timeout_ms"});
  CFD_TRYV(get(*tbl, "bind", "rest", out.bind));
  CFD_TRYV(get(*tbl, "port", "rest", out.port));
  CFD_TRYV(get(*tbl, "threads", "rest", out.threads));
  CFD_TRYV(get(*tbl, "blocking_threads", "rest", out.blocking_threads));
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
  if (out.blocking_threads > 1024) {
    return invalid("rest.blocking_threads must be <= 1024 (0 = automatic)");
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
  // Every token and accept_token of every peer, with where it was set.
  std::vector<std::pair<std::string, std::string>> secrets;
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
    for (const auto& [key, secret] :
         {std::pair{"token", &peer.token}, std::pair{"accept_token", &peer.accept_token}}) {
      const std::string field = std::format("{}.{}", where, key);
      if (const auto problem = token_problem(*secret); !problem.empty()) {
        return invalid(std::format("{} {}", field, problem));
      }
      // A shared accept_token makes two peers indistinguishable; a shared
      // token lets one peer pose as us to the other.
      const auto other = std::ranges::find(secrets, *secret, &decltype(secrets)::value_type::first);
      if (other != secrets.end()) {
        return invalid(
            std::format("{} is the same secret as {}; use a different one for "
                        "each peer and direction",
                        field, other->second));
      }
      secrets.emplace_back(*secret, field);
    }
    out.push_back(std::move(peer));
  }
  return {};
}

}  // namespace confide::config::detail
