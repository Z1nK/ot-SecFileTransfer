#pragma once
#include "common/error.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace confide::common {

// Most X-Forwarded-For entries client_address() looks at. A longer header is
// ignored (the peer address is used), so a client can't make each request
// parse thousands of entries.
inline constexpr std::size_t kMaxForwardedHops = 32;

// Canonical text of an IP literal ("2001:db8::1", "192.0.2.1"), with an
// IPv4-mapped IPv6 address ("::ffff:192.0.2.1") turned into plain IPv4 so one
// client always gets one text. nullopt if `text` is not an IP literal (host
// names, "1.2.3.4:80", "unknown", ...).
std::optional<std::string> canonical_ip(std::string_view text);

// A set of IP blocks, such as the trusted reverse proxies: "192.0.2.10" (one
// address), "10.0.0.0/8", "fd00::/8". Immutable once built; safe to share
// between threads.
class IpNetworks {
public:
  IpNetworks() = default;

  // `validation` naming the first bad entry: not an IP, prefix out of range,
  // or host bits set ("10.1.2.3/8" is refused rather than read as 10.0.0.0/8,
  // since in a trust list a typo there would trust a whole /8).
  static Result<IpNetworks> parse(std::span<const std::string> entries);

  bool empty() const { return nets_.empty(); }

  // False for anything that is not an IP literal.
  bool contains(std::string_view address) const;

private:
  struct Net {
    std::array<unsigned char, 16> bytes{};  // IPv4 uses the first 4
    unsigned prefix = 0;
    bool v4 = false;
  };
  static Result<Net> parse_one(std::string_view entry);

  std::vector<Net> nets_;
};

// The client's address for a request that arrived from `peer` (the socket's
// remote address) with `forwarded_for` (all X-Forwarded-For lines joined with
// ',', or "").
//
// The header is used only when `peer` is in `trusted`; anyone else could put
// any address there. Entries are read right to left, skipping trusted
// proxies; the first one that isn't trusted is the client. Entries left of it
// were written by the client and are never used. When the chain can't be
// followed (a malformed entry, or more than kMaxForwardedHops entries) the
// last trusted hop is returned, so a bad header never picks the address. A
// trusted peer that sends no header (health check, proxy misconfigured) is
// its own client. Every result is canonical_ip() text when it is an IP.
std::string client_address(std::string_view peer, std::string_view forwarded_for,
                           const IpNetworks& trusted);

}  // namespace confide::common
