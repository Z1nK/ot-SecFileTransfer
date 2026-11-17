#include "common/ip_network.hpp"

#include <boost/asio/ip/address.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <ranges>
#include <utility>

namespace confide::common {

namespace ip = boost::asio::ip;

namespace {

std::optional<ip::address> parse_ip(std::string_view text) {
  boost::system::error_code ec;
  auto address = ip::make_address(text, ec);
  if (ec) {
    return std::nullopt;
  }
  if (address.is_v6() && address.to_v6().is_v4_mapped()) {
    return ip::make_address_v4(ip::v4_mapped, address.to_v6());
  }
  return address;
}

std::string_view trim(std::string_view s) {
  constexpr std::string_view kSpace = " \t";
  const auto first = s.find_first_not_of(kSpace);
  if (first == std::string_view::npos) {
    return {};
  }
  return s.substr(first, s.find_last_not_of(kSpace) - first + 1);
}

// True when the first `prefix` bits of `a` and `b` are equal.
bool same_prefix(std::span<const unsigned char> a, std::span<const unsigned char> b,
                 unsigned prefix) {
  const unsigned whole = prefix / 8;
  if (!std::equal(a.begin(), a.begin() + whole, b.begin())) {
    return false;
  }
  const unsigned rest = prefix % 8;
  if (rest == 0) {
    return true;
  }
  const auto mask = static_cast<unsigned char>(0xFFU << (8 - rest));
  return (a[whole] & mask) == (b[whole] & mask);
}

}  // namespace

std::optional<std::string> canonical_ip(std::string_view text) {
  auto address = parse_ip(text);
  if (!address) {
    return std::nullopt;
  }
  return address->to_string();
}

Result<IpNetworks::Net> IpNetworks::parse_one(std::string_view entry) {
  const auto bad = [&](std::string_view why) {
    return std::unexpected(Error::make(ErrCode::validation, std::format("'{}': {}", entry, why)));
  };

  const auto slash = entry.find('/');
  const auto address = parse_ip(entry.substr(0, slash));
  if (!address) {
    return bad("not an IP address or CIDR block");
  }

  Net net;
  net.v4 = address->is_v4();
  if (net.v4) {
    std::ranges::copy(address->to_v4().to_bytes(), net.bytes.begin());
  } else {
    std::ranges::copy(address->to_v6().to_bytes(), net.bytes.begin());
  }
  net.prefix = net.v4 ? 32 : 128;

  if (slash != std::string_view::npos) {
    // parse_ip() turned a mapped address into IPv4, but its prefix is written
    // in IPv6 bits: "::ffff:10.0.0.0/104" is 10.0.0.0/8.
    const bool mapped = net.v4 && entry.substr(0, slash).find(':') != std::string_view::npos;
    const unsigned max_prefix = net.v4 && !mapped ? 32 : 128;
    const auto digits = entry.substr(slash + 1);
    const char* end = digits.data() + digits.size();
    unsigned prefix = 0;
    const auto [ptr, ec] = std::from_chars(digits.data(), end, prefix);
    if (digits.empty() || ec != std::errc{} || ptr != end || prefix > max_prefix) {
      return bad(std::format("prefix must be 0..{}", max_prefix));
    }
    if (mapped) {
      if (prefix < 96) {
        return bad("an IPv4-mapped block needs a prefix of 96..128");
      }
      prefix -= 96;
    }
    net.prefix = prefix;
  }

  const std::span bytes{net.bytes.data(), net.v4 ? 4U : 16U};
  std::array<unsigned char, 16> masked{};
  for (unsigned bit = 0; bit < net.prefix; ++bit) {
    masked[bit / 8] |= static_cast<unsigned char>(bytes[bit / 8] & (0x80U >> (bit % 8)));
  }
  if (!std::equal(bytes.begin(), bytes.end(), masked.begin())) {
    std::string block;
    if (net.v4) {
      ip::address_v4::bytes_type b{};
      std::copy_n(masked.begin(), b.size(), b.begin());
      block = ip::address_v4{b}.to_string();
    } else {
      block = ip::address_v6{masked}.to_string();
    }
    return bad(std::format("host bits set; the block is {}/{}", block, net.prefix));
  }
  return net;
}

Result<IpNetworks> IpNetworks::parse(std::span<const std::string> entries) {
  IpNetworks out;
  out.nets_.reserve(entries.size());
  for (const auto& entry : entries) {
    CFD_TRY(net, parse_one(entry));
    out.nets_.push_back(net);
  }
  return out;
}

bool IpNetworks::contains(std::string_view address) const {
  const auto parsed = parse_ip(address);
  if (!parsed) {
    return false;
  }
  std::array<unsigned char, 16> bytes{};
  const bool v4 = parsed->is_v4();
  if (v4) {
    std::ranges::copy(parsed->to_v4().to_bytes(), bytes.begin());
  } else {
    std::ranges::copy(parsed->to_v6().to_bytes(), bytes.begin());
  }
  return std::ranges::any_of(nets_, [&](const Net& net) {
    return net.v4 == v4 && same_prefix(net.bytes, bytes, net.prefix);
  });
}

std::string client_address(std::string_view peer, std::string_view forwarded_for,
                           const IpNetworks& trusted) {
  std::string last = canonical_ip(peer).value_or(std::string{peer});
  if (trusted.empty() || !trusted.contains(last)) {
    return last;
  }

  std::vector<std::string_view> hops;
  for (const auto part : std::views::split(forwarded_for, ',')) {
    const auto hop = trim(std::string_view{part.begin(), part.end()});
    if (hop.empty()) {
      continue;  // "a,,b" or a trailing comma
    }
    if (hops.size() == kMaxForwardedHops) {
      return last;
    }
    hops.push_back(hop);
  }

  for (const auto hop : std::views::reverse(hops)) {
    auto address = canonical_ip(hop);
    if (!address) {
      return last;
    }
    if (!trusted.contains(*address)) {
      return std::move(*address);
    }
    last = std::move(*address);
  }
  return last;  // every hop was a trusted proxy: the innermost one we know
}

}  // namespace confide::common
