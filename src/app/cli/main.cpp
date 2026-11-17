// `ftc`: command-line client for the REST API (Architecture §2, §4 `client`).
//
//   ftc -s https://siteA:8080 -u alice send --to bob@siteB ~/data/report
//   ftc -s https://siteB:8080 -u bob inbox
//   ftc -s https://siteB:8080 -u bob get <id> ~/downloads
//
// Server, user and CA bundle come from, highest priority first: the
// command line (-s, -u, --ca-file), the environment (FTC_SERVER, FTC_USER,
// FTC_CA_FILE), the user's config file ($XDG_CONFIG_HOME/ftc/ftc.toml),
// the system one (/etc/ftc/ftc.toml); see client/settings.hpp. --config /
// FTC_CONFIG names one file to read instead of those two. `ftc config`
// shows what was picked from where.
//
// The first command (or `ftc login`) asks for the password and trades it for
// a token (POST /auth/token), kept in a private file (cli::TokenCache).
// Later commands send only the token; when it expires, ftc asks again. The
// password comes from FTC_PASSWORD (development only), or is asked for on
// the terminal (first line of stdin if stdin is not a terminal), so it
// never appears in `ps` or in a config file. An http:// server needs
// --allow-http (command line only), since the password and token would
// travel unencrypted.
//
// main.cpp takes the global options; dispatch.cpp the command and its
// arguments; commands/ the commands themselves.

#include "cli.hpp"
#include "dispatch.hpp"
#include "platform.hpp"

#include <boost/program_options.hpp>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace po = boost::program_options;

using namespace confide;

namespace {

// Names of one source of the global options, for Settings::Origins.
struct Source {
  std::string_view server;
  std::string_view user;
  std::string_view ca_file;
};
constexpr Source kFromCli{.server = "--server", .user = "--user", .ca_file = "--ca-file"};
constexpr Source kFromEnv{
    .server = "env FTC_SERVER", .user = "env FTC_USER", .ca_file = "env FTC_CA_FILE"};

std::string env_option(const std::string& env) {
  if (env == "FTC_SERVER") {
    return "server";
  }
  if (env == "FTC_USER") {
    return "user";
  }
  if (env == "FTC_CA_FILE") {
    return "ca-file";
  }
  if (env == "FTC_CONFIG") {
    return "config";
  }
  return {};
}

client::Settings layer(const po::variables_map& vm, const Source& src) {
  client::Settings s;
  if (vm.contains("server")) {
    s.server = vm["server"].as<std::string>();
    s.from.server = src.server;
  }
  if (vm.contains("user")) {
    s.user = vm["user"].as<std::string>();
    s.from.user = src.user;
  }
  if (vm.contains("ca-file")) {
    s.ca_file = fs::path{vm["ca-file"].as<std::string>()};
    s.from.ca_file = src.ca_file;
  }
  return s;
}

std::optional<std::string> option(const po::variables_map& cli, const po::variables_map& env,
                                  const char* name) {
  for (const auto* vm : {&cli, &env}) {
    if (vm->contains(name)) {
      return (*vm)[name].as<std::string>();
    }
  }
  return std::nullopt;
}

// The config file layer: the one file named by --config / FTC_CONFIG (it
// must exist; empty = no file at all), else the system file overlaid with
// the user's, those that exist. Warnings go to stderr.
common::Result<client::Settings> file_layer(const std::optional<std::string>& explicit_file,
                                            std::vector<fs::path>& read) {
  client::Settings merged;
  auto take = [&](const fs::path& path, client::LoadedSettings loaded) {
    for (const auto& w : loaded.warnings) {
      std::cerr << "ftc: warning: " << w << '\n';
    }
    merged = client::overlay(std::move(merged), loaded.values);
    read.push_back(path);
  };
  if (explicit_file && explicit_file->empty()) {
    return merged;
  }
  if (explicit_file) {
    CFD_TRY(loaded, client::load_settings_file(*explicit_file));
    take(*explicit_file, std::move(loaded));
    return merged;
  }
  for (const auto& path : client::default_settings_files()) {
    CFD_TRY(loaded, client::load_settings_file_if_exists(path));
    if (loaded) {
      take(path, std::move(*loaded));
    }
  }
  return merged;
}

}  // namespace

int main(int argc, char** argv) {
  po::options_description global("Options");
  global.add_options()("help,h", "show this help")(
      "server,s", po::value<std::string>()->value_name("URL"),
      "server, e.g. https://siteA:8080 (env FTC_SERVER)")(
      "user,u", po::value<std::string>()->value_name("NAME"),
      "user name (env FTC_USER; default: current OS user)")(
      "ca-file", po::value<std::string>()->value_name("PATH"),
      "PEM CA bundle for https (env FTC_CA_FILE; default: system CAs)")(
      "config", po::value<std::string>()->value_name("PATH"),
      "read only this config file, \"\" for none (env FTC_CONFIG; default: "
      "/etc/ftc/ftc.toml, then ~/.config/ftc/ftc.toml)")(
      "allow-http",
      "allow a plain http:// server; the password is sent unencrypted "
      "(local testing only)");
  po::options_description hidden;
  hidden.add_options()("command", po::value<std::string>())("args",
                                                            po::value<std::vector<std::string>>());
  po::options_description all;
  all.add(global).add(hidden);
  po::positional_options_description pos;
  pos.add("command", 1).add("args", -1);

  // Command line and environment apart, so `ftc config` can tell which
  // one a value came from; the command line wins.
  po::variables_map vm;
  po::variables_map env;
  std::vector<std::string> args;
  try {
    // Everything from the command name on belongs to the command.
    const auto parsed =
        po::command_line_parser(argc, argv)
            .options(all)
            .positional(pos)
            .style(po::command_line_style::default_style & ~po::command_line_style::allow_guessing)
            .allow_unregistered()
            .run();
    po::store(parsed, vm);
    po::store(po::parse_environment(global, env_option), env);
    po::notify(vm);
    po::notify(env);

    args = po::collect_unrecognized(parsed.options, po::include_positional);
    if (!args.empty()) {
      args.erase(args.begin());  // the command name itself
    }
  } catch (const po::error& e) {
    std::cerr << "ftc: " << e.what() << "\n\n";
    cli::usage(global);
    return cli::kExitUsage;
  }

  if (!vm.contains("command")) {
    cli::usage(global);
    return vm.contains("help") ? cli::kExitOk : cli::kExitUsage;
  }
  if (vm.contains("help")) {
    args.emplace_back("--help");
  }

  // Lowest priority first: files, environment, command line.
  std::vector<fs::path> read;
  auto files = file_layer(option(vm, env, "config"), read);
  if (!files) {
    std::cerr << "ftc: " << files.error().detail << '\n';
    return cli::kExitUsage;
  }
  const auto s = client::overlay(client::overlay(std::move(*files), layer(env, kFromEnv)),
                                 layer(vm, kFromCli));

  // An empty value, from wherever, means the default.
  cli::Global g{
      .server = s.server.value_or(std::string{}),
      .user = s.user.value_or(std::string{}),
      .ca_file = s.ca_file && !s.ca_file->empty() ? s.ca_file : std::nullopt,
      .allow_http = vm.contains("allow-http"),
      .from = s.from,
      .config_files = std::move(read),
  };
  if (g.server.empty()) {
    g.from.server.clear();
  }
  if (g.user.empty()) {
    g.user = cli::system_user();
    g.from.user.clear();
  }
  if (!g.ca_file) {
    g.from.ca_file.clear();
  }
  return cli::run_command(vm["command"].as<std::string>(), args, g);
}
