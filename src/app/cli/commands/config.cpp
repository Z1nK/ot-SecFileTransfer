// ftc config: the server, user and CA bundle ftc would use, and where each
// came from (command line, environment, config file, default). No server
// access, so it also works before the first login.
#include "commands.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <iostream>
#include <string>
#include <string_view>

namespace confide::cli {

namespace {

constexpr std::array<std::string_view, 3> kKeys{"server", "user", "ca_file"};

void config_options(po::options_description& desc, po::options_description& /*hidden*/,
                    po::positional_options_description& /*pos*/) {
  desc.add_options()("get", po::value<std::string>()->value_name("KEY"),
                     "print only the value of KEY (server, user, ca_file), "
                     "empty if not set, for scripts");
}

struct Entry {
  std::string value;          // empty = not set
  std::string origin;         // empty = default
  std::string_view fallback;  // shown when value is empty
};

Entry entry(const Global& g, std::string_view key) {
  if (key == "server") {
    return {.value = g.server, .origin = g.from.server, .fallback = "(not set)"};
  }
  if (key == "user") {
    return {.value = g.user, .origin = g.from.user, .fallback = "(unknown)"};
  }
  return {.value = g.ca_file ? g.ca_file->string() : std::string{},
          .origin = g.from.ca_file,
          .fallback = "(system CAs)"};
}

Result<void> run_config(const Global& global, const po::variables_map& vm) {
  if (vm.contains("get")) {
    const auto& key = vm["get"].as<std::string>();
    if (std::ranges::find(kKeys, key) == kKeys.end()) {
      return fail(ErrCode::validation,
                  std::format("unknown key '{}' (server, user, ca_file)", key));
    }
    std::cout << entry(global, key).value << '\n';
    return {};
  }

  for (const auto key : kKeys) {
    const auto e = entry(global, key);
    std::cout << std::format("{:<8} = {:<40} # {}\n", key,
                             e.value.empty() ? std::string{e.fallback} : e.value,
                             e.origin.empty() ? "default" : e.origin);
  }
  std::cout << "allow_http = " << (global.allow_http ? "yes" : "no") << " (command line only)\n";
  if (global.config_files.empty()) {
    std::cout << "config files: none\n";
  } else {
    std::cout << "config files, later ones win:\n";
    for (const auto& f : global.config_files) {
      std::cout << "  " << f.string() << '\n';
    }
  }
  return {};
}

}  // namespace

const Command kConfig{
    .name = "config", .synopsis = " [--get KEY]", .options = config_options, .run = run_config};

}  // namespace confide::cli
