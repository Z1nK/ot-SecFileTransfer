#pragma once
#include "http_client.hpp"

#include <common/error.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace confide::cli {

// The user token `ftc login` got from one server, for one user, kept in
//
//   $XDG_STATE_HOME/ftc/tokens/<user>@<host>_<port>   (~/.local/state if unset)
//
// The folders are created 0700 and the file 0600. Like ssh with its keys,
// a token file or folder that others could read or replace is refused
// rather than used.
class TokenCache {
public:
  // `validation` if neither XDG_STATE_HOME nor HOME is set.
  static Result<TokenCache> for_server(const ServerUrl& url, std::string_view user);

  // nullopt if there is no token yet. `validation` if the file or its folder
  // is not private to this user, `io` if it can't be read.
  Result<std::optional<std::string>> load() const;

  // Replaces the file atomically (temp file + rename).
  Result<void> save(std::string_view token) const;

  // No error if there is no file.
  Result<void> remove() const;

  const std::filesystem::path& path() const { return file_; }

private:
  explicit TokenCache(std::filesystem::path file) : file_(std::move(file)) {}

  // Creates the token folder (0700) if needed and checks it is private.
  Result<void> prepare_dir() const;

  std::filesystem::path file_;
};

}  // namespace confide::cli
