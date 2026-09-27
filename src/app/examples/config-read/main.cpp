// Usage example for config/config.hpp: load a TOML file with ConfigLoader,
// handle the Result, read the typed sections, look up users and peers.
//
//   example-config-read                 # reads test_config.toml next to the binary
//   example-config-read path/to/file.toml

#include <config/config.hpp>

#include <filesystem>
#include <iostream>
#include <span>
#include <string_view>

using confide::common::to_string;
using confide::config::Config;
using confide::config::ConfigLoader;

namespace fs = std::filesystem;

namespace {

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

  std::cout << "users (" << cfg.auth().users.size() << "), token ttl " << cfg.auth().token_ttl_s
            << "s:\n";
  for (const auto& u : cfg.auth().users) {
    std::cout << "  - " << u.name << '\n';
  }

  std::cout << "peers (" << cfg.peers().size() << "):\n";
  for (const auto& p : cfg.peers()) {
    std::cout << "  - " << p.name << " -> " << p.host << ':' << p.port << '\n';
  }
}

// resolving `user@server` the way the API will on create.
void lookup(const Config& cfg, std::string_view user, std::string_view server) {
  std::cout << "lookup " << user << '@' << server << ": ";
  if (server == cfg.instance_name()) {
    std::cout << (cfg.find_user(user) != nullptr ? "local user" : "destination unknown (user)");
  } else if (const auto* peer = cfg.find_peer(server)) {
    std::cout << "forward to " << peer->host << ':' << peer->port;
  } else {
    std::cout << "destination unknown (server)";
  }
  std::cout << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  const std::span args(argv, static_cast<std::size_t>(argc));
  // Default: the test config copied next to this binary at build time, so
  // the example works from any working directory.
  const fs::path path = args.size() > 1 ? fs::path(args[1])
                                        : fs::absolute(args[0]).parent_path() / "test_config.toml";

  auto cfg = ConfigLoader::from_file(path);
  if (!cfg) {
    std::cerr << "failed to load " << path.string() << ": [" << to_string(cfg.error().code) << "] "
              << cfg.error().detail << '\n';
    return 1;
  }

  // Warnings are non-fatal; the caller decides where to print them.
  for (const auto& w : cfg->warnings()) {
    std::cout << "warning: " << w << '\n';
  }

  std::cout << "\n== " << path.string() << " ==\n";
  print(*cfg);

  std::cout << '\n';
  lookup(*cfg, "bob", cfg->instance_name());
  lookup(*cfg, "carol", cfg->instance_name());
  lookup(*cfg, "bob", "siteB");
  lookup(*cfg, "bob", "siteZ");

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
  return 0;
}
