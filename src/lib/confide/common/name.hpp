#pragma once
#include <algorithm>
#include <string_view>

namespace confide::common {

// Names end up in routing (`bob@siteB`), Basic auth (`user:pass`), one-line
// listings and logs, so keep them to a conservative charset: no '@', ':',
// '/', spaces or control characters. Applies to instance, peer, user and
// group names, local or from LDAP, and to the sender a peer names in a push.
inline bool is_valid_name(std::string_view s) {
  return !s.empty() && s.size() <= 64 && std::ranges::all_of(s, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'
           || c == '_' || c == '.';
  });
}

}  // namespace confide::common
