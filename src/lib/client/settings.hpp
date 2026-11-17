#pragma once
// Client settings (server, user, CA bundle) from config files, layered with
// the environment and the command line by the front end:
//
//   system file  <  user file  <  environment  <  command line
//
//   /etc/ftc/ftc.toml                   %PROGRAMDATA%\ftc\ftc.toml   (system)
//   $XDG_CONFIG_HOME/ftc/ftc.toml       %APPDATA%\ftc\ftc.toml       (user)
//   (~/.config if unset)
//
// Every file is optional. A file is TOML with top-level keys only:
//
//   server = "https://ftc.example.org:8443"
//   user = "alice"
//   ca_file = "ca.pem"        # relative paths: against the file's folder
//
// The password and --allow-http are never taken from a file.
#include "client.hpp"

#include <filesystem>
#include <istream>
#include <optional>
#include <string>
#include <vector>

namespace confide::client {

// One layer of settings; nullopt = not set in this layer. An empty value is
// set: it overrides lower layers and means "the default" (system CAs, the
// OS user, no server).
struct Settings {
  std::optional<std::string> server;
  std::optional<std::string> user;
  std::optional<std::filesystem::path> ca_file;

  // Where each value came from ("/etc/ftc/ftc.toml", "env FTC_USER", ...);
  // empty while the value is unset.
  struct Origins {
    std::string server;
    std::string user;
    std::string ca_file;
  } from;
};

struct LoadedSettings {
  Settings values;
  std::vector<std::string> warnings;  // unknown keys and the like, ignored
};

// Parses one settings file. `validation` naming the file if it can't be
// read, is not valid TOML or a value has the wrong type.
Result<LoadedSettings> load_settings_file(const std::filesystem::path& path);

// As load_settings_file(), but nullopt if there is no such file.
Result<std::optional<LoadedSettings>> load_settings_file_if_exists(
    const std::filesystem::path& path);

// Parses settings text; `source` names it in messages and origins,
// relative paths are resolved against `base_dir` (none if empty).
Result<LoadedSettings> parse_settings(std::istream& in, const std::string& source,
                                      const std::filesystem::path& base_dir);

// `low` with every value that `high` sets replaced, origin included.
Settings overlay(Settings low, const Settings& high);

// The files read when none is named explicitly, lowest priority first:
// the system file, then the user file (left out if neither XDG_CONFIG_HOME
// nor HOME, on Windows APPDATA, is set).
std::vector<std::filesystem::path> default_settings_files();

}  // namespace confide::client
