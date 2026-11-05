#include "token_cache.hpp"

#include "token_cache_detail.hpp"

#include <format>

// The OS-independent part of TokenCache: the file name. Folder creation,
// the privacy checks and the file I/O are in token_cache_posix.cpp or
// token_cache_win.cpp.

namespace confide::client {

namespace {

// Percent-encodes everything but [A-Za-z0-9.-], so a user or host name
// can't add path parts and the '@' and '_' separators stay unambiguous.
std::string file_part(std::string_view text) {
  constexpr std::string_view kHex = "0123456789ABCDEF";
  std::string out;
  for (const char c : text) {
    const auto u = static_cast<unsigned char>(c);
    if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || c == '.'
        || c == '-') {
      out.push_back(c);
    } else {
      out.push_back('%');
      out.push_back(kHex[u >> 4]);
      out.push_back(kHex[u & 0x0F]);
    }
  }
  return out;
}

}  // namespace

Result<TokenCache> TokenCache::for_server(const ServerUrl& url, std::string_view user, bool admin) {
  CFD_TRY(state, detail::state_dir());
  return TokenCache(state / "ftc" / "tokens"
                    / std::format("{}@{}_{}{}", file_part(user), file_part(url.host), url.port,
                                  admin ? ".admin" : ""));
}

}  // namespace confide::client
