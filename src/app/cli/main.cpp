// `ftc`: command-line client for the REST API (Architecture §2, §4 `client`).
//
//   ftc -s https://siteA:8080 -u alice send --to bob@siteB ~/data/report
//   ftc -s https://siteB:8080 -u bob inbox
//   ftc -s https://siteB:8080 -u bob get <id> ~/downloads
//
// The first command (or `ftc login`) asks for the password and trades it for
// a token (POST /auth/token), kept in a private file (cli::TokenCache).
// Later commands send only the token; when it expires, ftc asks again. The
// password comes from FTC_PASSWORD (development only), or is asked for on
// the terminal (first line of stdin if stdin is not a terminal), so it
// never appears in `ps`. An http:// server needs --allow-http, since the
// password and token would travel unencrypted.
//
// main.cpp takes the global options; dispatch.cpp the command and its
// arguments; commands/ the commands themselves.

#include "cli.hpp"
#include "dispatch.hpp"

#include <boost/program_options.hpp>

#ifndef _WIN32
#include <pwd.h>
#include <unistd.h>
#endif

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace po = boost::program_options;

using namespace confide;

namespace {

// The account ftc runs as, from the passwd database (NSS, so LDAP accounts
// too), not from $USER. Empty if the lookup fails.
#ifdef _WIN32
// TODO: GetUserNameW(). Until then --user or FTC_USER is required.
std::string system_user() { return {}; }
#else
std::string system_user() {
  const long hint = sysconf(_SC_GETPW_R_SIZE_MAX);
  std::vector<char> buf(hint > 0 ? static_cast<std::size_t>(hint) : 16384);
  passwd pwd{};
  passwd* found = nullptr;
  if (getpwuid_r(geteuid(), &pwd, buf.data(), buf.size(), &found) != 0 || found == nullptr) {
    return {};
  }
  return found->pw_name;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  po::options_description global("Options");
  global.add_options()("help,h", "show this help")(
      "server,s", po::value<std::string>()->value_name("URL"),
      "server, e.g. https://siteA:8080 (env FTC_SERVER)")(
      "user,u", po::value<std::string>()->value_name("NAME"), "user name (env FTC_USER; default: current OS user)")(
      "ca-file", po::value<std::string>()->value_name("PATH"),
      "PEM CA bundle for https (env FTC_CA_FILE; default: system CAs)")(
      "allow-http", "allow a plain http:// server; the password is sent unencrypted "
                    "(local testing only)");
  po::options_description hidden;
  hidden.add_options()("command", po::value<std::string>())("args",
                                                            po::value<std::vector<std::string>>());
  po::options_description all;
  all.add(global).add(hidden);
  po::positional_options_description pos;
  pos.add("command", 1).add("args", -1);

  po::variables_map vm;
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
    po::store(po::parse_environment(global,
                                    [](const std::string& env) -> std::string {
                                      if (env == "FTC_SERVER") {
                                        return "server";
                                      }
                                      if (env == "FTC_USER") {
                                        return "user";
                                      }
                                      if (env == "FTC_CA_FILE") {
                                        return "ca-file";
                                      }
                                      return {};
                                    }),
              vm);
    po::notify(vm);

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

  cli::Global g{
      .server = vm.contains("server") ? vm["server"].as<std::string>() : std::string{},
      .user = vm.contains("user") ? vm["user"].as<std::string>() : system_user(),
      .ca_file = vm.contains("ca-file") && !vm["ca-file"].as<std::string>().empty()
                     ? std::optional<fs::path>{vm["ca-file"].as<std::string>()}
                     : std::nullopt,
      .allow_http = vm.contains("allow-http"),
  };
  return cli::run_command(vm["command"].as<std::string>(), args, g);
}
