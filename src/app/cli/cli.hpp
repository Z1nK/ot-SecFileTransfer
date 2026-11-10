#pragma once
// Shared by the ftc sources: global options, exit codes, error helpers.
#include <client/client.hpp>
#include <common/error.hpp>

#include <filesystem>
#include <format>
#include <optional>
#include <string>

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

// Options shared by every command, from the command line or the environment.
struct Global {
  std::string server;
  std::string user;
  std::optional<std::filesystem::path> ca_file;
  bool allow_http = false;
};

inline std::string describe(const Error& err) {
  return std::format("[{}] {}", common::to_string(err.code), err.detail);
}

}  // namespace confide::cli
