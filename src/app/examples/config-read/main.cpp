// Usage example for config/config.hpp: load a TOML file with ConfigLoader,
// handle the Result, read the typed sections, look up users, groups and peers.
//
//   example-config-read                 # reads test_config.toml and
//                                       # test_config_ldap.toml next to the binary
//   example-config-read path/to/file.toml [more.toml ...]

#include <config/config.hpp>

#include <filesystem>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

using confide::common::to_string;
using confide::config::AuthProvider;
using confide::config::Config;
using confide::config::ConfigLoader;

namespace fs = std::filesystem;

namespace {

void print_auth(const Config& cfg) {
  const auto& auth = cfg.auth();
  std::cout << "auth          : provider="
            << (auth.provider == AuthProvider::ldap ? "ldap" : "local")
            << ", token ttl=" << auth.token_ttl_s << "s, idle=" << auth.token_idle_s << "s\n";

  if (auth.provider == AuthProvider::ldap) {
    // Plain data only: users/groups are looked up in the directory at runtime.
    const auto& ldap = auth.ldap;
    std::cout << "ldap          : preset=" << ldap.preset << ", uri=" << ldap.uri
              << ", start_tls=" << (ldap.start_tls ? "on" : "off")
              << ", timeout=" << ldap.timeout_s << "s, cache=" << ldap.cache_ttl_s << "s\n";
    std::cout << "  bind_dn     : " << (ldap.bind_dn.empty() ? "(anonymous)" : ldap.bind_dn)
              << '\n';
    std::cout << "  users       : " << ldap.user_base_dn << '\n';
    std::cout << "  groups      : "
              << (ldap.group_base_dn.empty() ? "(none)" : ldap.group_base_dn) << '\n';
    return;
  }

  if (!auth.users_file.empty()) {
    std::cout << "users_file    : " << auth.users_file.string() << '\n';
  }
  if (!auth.groups_file.empty()) {
    std::cout << "groups_file   : " << auth.groups_file.string() << '\n';
  }

  std::cout << "users (" << auth.users.size() << "):\n";
  for (const auto& u : auth.users) {
    std::cout << "  - " << u.name << '\n';
  }

  std::cout << "groups (" << auth.groups.size() << "):\n";
  for (const auto& g : auth.groups) {
    std::cout << "  - " << g.name << ':';
    for (const auto& m : g.members) {
      std::cout << ' ' << m;
    }
    std::cout << '\n';
  }
}

void print(const Config& cfg) {
  std::cout << "instance_name : " << cfg.instance_name() << '\n';
  std::cout << "storage.root  : " << cfg.storage().root.string() << '\n';

  const auto& rest = cfg.rest();
  std::cout << "rest          : " << rest.bind << ':' << rest.port << ", threads=" << rest.threads
            << ", timeout=" << rest.request_timeout_ms << "ms\n";

  const auto& tls = cfg.tls();
  std::cout << "tls           : " << (tls.enabled ? "on" : "off");
  if (tls.enabled) {
    std::cout << " (cert=" << tls.cert_path.string() << ", key=" << tls.key_path.string() << ')';
  }
  std::cout << '\n';

  std::cout << "log           : level=" << confide::logging::to_string(cfg.log().level)
            << ", dir=" << cfg.log().dir.string() << '\n';

  const auto& ret = cfg.retention();
  std::cout << "retention     : default=" << ret.default_days << "d, max=" << ret.max_days
            << "d, sweep every " << ret.sweep_interval_s << "s\n";

  print_auth(cfg);

  std::cout << "peers (" << cfg.peers().size() << "):\n";
  for (const auto& p : cfg.peers()) {
    std::cout << "  - " << p.name << " -> " << p.host << ':' << p.port << '\n';
  }
}

// resolving `user@server` the way the API will on create. With LDAP the
// config holds no users, so local names are only known to the directory.
void lookup(const Config& cfg, std::string_view user, std::string_view server) {
  std::cout << "lookup " << user << '@' << server << ": ";
  if (server == cfg.instance_name()) {
    if (cfg.auth().provider == AuthProvider::ldap) {
      std::cout << "local, resolved via LDAP";
    } else {
      std::cout << (cfg.find_user(user) != nullptr    ? "local user"
                    : cfg.find_group(user) != nullptr ? "local group"
                                                      : "destination unknown (user)");
    }
  } else if (const auto* peer = cfg.find_peer(server)) {
    std::cout << "forward to " << peer->host << ':' << peer->port;
  } else {
    std::cout << "destination unknown (server)";
  }
  std::cout << '\n';
}

// false if the file failed to load.
bool show(const fs::path& path) {
  std::cout << "\n== " << path.string() << " ==\n";
  auto cfg = ConfigLoader::from_file(path);
  if (!cfg) {
    std::cerr << "failed to load " << path.string() << ": [" << to_string(cfg.error().code)
              << "] " << cfg.error().detail << '\n';
    return false;
  }

  // Warnings are non-fatal; the caller decides where to print them.
  for (const auto& w : cfg->warnings()) {
    std::cout << "warning: " << w << '\n';
  }

  print(*cfg);

  std::cout << '\n';
  lookup(*cfg, "bob", cfg->instance_name());
  lookup(*cfg, "team", cfg->instance_name());
  lookup(*cfg, "mallory", cfg->instance_name());
  lookup(*cfg, "bob", "siteB");
  lookup(*cfg, "bob", "siteZ");
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const std::span args(argv, static_cast<std::size_t>(argc));

  // Default: the test configs copied next to this binary at build time, so
  // the example works from any working directory.
  std::vector<fs::path> paths;
  if (args.size() > 1) {
    for (const char* a : args.subspan(1)) {
      paths.emplace_back(a);
    }
  } else {
    const fs::path bin = fs::absolute(args[0]).parent_path();
    paths = {bin / "test_config.toml", bin / "test_config_ldap.toml"};
  }

  bool ok = true;
  for (const auto& p : paths) {
    ok = show(p) && ok;
  }

  // Validation errors come back as ErrCode::validation with a readable detail.
  std::cout << "\n== invalid config ==\n";
  auto bad = ConfigLoader::from_string(R"(
instance_name = "siteA"
[storage]
root = "/tmp/x"
[tls]
enabled = false
[[user]]
name = "alice"
password = "secret"
)");
  if (!bad) {
    std::cout << '[' << to_string(bad.error().code) << "] " << bad.error().detail << '\n';
  }
  return ok ? 0 : 1;
}
