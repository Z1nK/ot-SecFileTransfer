#pragma once
#include <common/error.hpp>
#include <net/http_client.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace confide::client {

using common::Result;
using net::ServerUrl;

// The last recipients `ftc send` was used with, for one server and user,
// so front ends (file manager menus, a GUI) can offer them. Kept next to
// the tokens of TokenCache:
//
//   $XDG_STATE_HOME/ftc/recent/<user>@<host>_<port>   (~/.local/state if unset)
//   %LOCALAPPDATA%\ftc\recent\<user>@<host>_<port>    (Windows)
//
// Plain text, one recipient per line, most recent first, at most kMax.
// The ftc folder itself is never created here: TokenCache creates it
// private on login, and a folder made with default permissions would make
// it refuse the tokens. push() fails if it is not there yet.
class RecentRecipients {
public:
  static constexpr std::size_t kMax = 10;

  // `validation` if neither XDG_STATE_HOME nor HOME (Windows: LOCALAPPDATA)
  // is set.
  static Result<RecentRecipients> for_server(const ServerUrl& url, std::string_view user);

  // Most recent first; empty if there is no file yet. `io` if it can't be read.
  Result<std::vector<std::string>> load() const;

  // Puts `recipient` first (dropping an older copy) and keeps kMax.
  // Replaces the file atomically (temp file + rename).
  Result<void> push(std::string_view recipient) const;

  const std::filesystem::path& path() const { return file_; }

private:
  explicit RecentRecipients(std::filesystem::path file) : file_(std::move(file)) {}

  std::filesystem::path file_;
};

namespace detail {

// `list` with `recipient` moved to the front, trimmed to `max`. A recipient
// that is empty or has a line break is ignored.
std::vector<std::string> with_recent(std::vector<std::string> list, std::string_view recipient,
                                     std::size_t max = RecentRecipients::kMax);

}  // namespace detail

}  // namespace confide::client
