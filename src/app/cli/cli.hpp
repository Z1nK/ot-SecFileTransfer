#pragma once
// Shared by the ftc sources: global options, exit codes, error helpers.
#include <client/client.hpp>
#include <client/settings.hpp>
#include <common/error.hpp>

#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace confide::cli {

// The client library (token cache, local files, replies, outgoing
// transactions) is used unqualified throughout ftc.
using namespace confide::client;

using common::ErrCode;
using common::Error;
using common::Result;

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;

// Options shared by every command: from the command line, else the
// environment, else the config files (client/settings.hpp), else defaults.
struct Global {
  std::string server;
  std::string user;
  std::optional<std::filesystem::path> ca_file;
  bool allow_http = false;

  // For `ftc config`: where each value came from ("--server", "env
  // FTC_SERVER", a file path; empty = default) and the files that were read.
  Settings::Origins from;
  std::vector<std::filesystem::path> config_files;
};

inline std::string describe(const Error& err) {
  return std::format("[{}] {}", common::to_string(err.code), err.detail);
}

}  // namespace confide::cli
