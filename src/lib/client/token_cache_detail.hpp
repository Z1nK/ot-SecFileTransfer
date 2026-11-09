#pragma once
// Shared by token_cache.cpp and its per-OS half: token_cache_posix.cpp or
// token_cache_win.cpp (CMake builds one of the two), and recent_recipients.cpp.
#include "client.hpp"

#include <net/http_client.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace confide::client::detail {

inline constexpr std::size_t kMaxTokenBytes = 4096;

// The per-user folder that holds ftc/tokens/: $XDG_STATE_HOME or
// ~/.local/state on POSIX, %LOCALAPPDATA% on Windows.
Result<std::filesystem::path> state_dir();

// Percent-encodes everything but [A-Za-z0-9.-], so a user or host name
// can't add path parts and the '@' and '_' separators stay unambiguous.
std::string file_part(std::string_view text);

// "<user>@<host>_<port>", the per-server file name under ftc/tokens/ and
// ftc/recent/.
std::string server_file_name(const net::ServerUrl& url, std::string_view user);

// The token in the file's text without the trailing newline; nullopt if
// nothing is left.
inline std::optional<std::string> token_from(std::string_view text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
    text.remove_suffix(1);
  }
  if (text.empty()) {
    return std::nullopt;
  }
  return std::string{text};
}

}  // namespace confide::client::detail
