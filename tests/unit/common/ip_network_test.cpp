#include <common/ip_network.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using confide::common::canonical_ip;
using confide::common::client_address;
using confide::common::ErrCode;
using confide::common::IpNetworks;
using confide::common::kMaxForwardedHops;

namespace {

IpNetworks nets(std::vector<std::string> entries) {
  auto n = IpNetworks::parse(entries);
  EXPECT_TRUE(n.has_value()) << n.error().detail;
  return n.value_or(IpNetworks{});
}

void expect_rejected(const std::string& entry, std::string_view detail_part) {
  const std::vector<std::string> one{entry};
  auto n = IpNetworks::parse(one);
  ASSERT_FALSE(n.has_value()) << entry;
  EXPECT_EQ(n.error().code, ErrCode::validation);
  EXPECT_NE(n.error().detail.find(entry), std::string::npos) << n.error().detail;
  EXPECT_NE(n.error().detail.find(detail_part), std::string::npos) << n.error().detail;
}

}  // namespace

TEST(CanonicalIp, NormalizesIpLiterals) {
  EXPECT_EQ(canonical_ip("192.0.2.1"), "192.0.2.1");
  EXPECT_EQ(canonical_ip("2001:DB8:0:0::1"), "2001:db8::1");
  EXPECT_EQ(canonical_ip("::ffff:192.0.2.1"), "192.0.2.1");  // one client, one text
  EXPECT_EQ(canonical_ip("::1"), "::1");
}

TEST(CanonicalIp, RejectsWhatIsNotAnIp) {
  for (const auto* s : {"", "unknown", "host.example", "192.0.2.1:8080", "[2001:db8::1]",
                        "192.0.2.256", " 192.0.2.1"}) {
    EXPECT_FALSE(canonical_ip(s).has_value()) << s;
  }
}

TEST(IpNetworks, EmptyContainsNothing) {
  const IpNetworks none;
  EXPECT_TRUE(none.empty());
  EXPECT_FALSE(none.contains("127.0.0.1"));
}

TEST(IpNetworks, SingleAddresses) {
  const auto n = nets({"192.0.2.10", "2001:db8::1"});
  EXPECT_TRUE(n.contains("192.0.2.10"));
  EXPECT_FALSE(n.contains("192.0.2.11"));
  EXPECT_TRUE(n.contains("2001:db8::1"));
  EXPECT_TRUE(n.contains("2001:DB8::0001"));
  EXPECT_FALSE(n.contains("2001:db8::2"));
  EXPECT_TRUE(n.contains("::ffff:192.0.2.10"));  // mapped form of a listed IPv4
  EXPECT_FALSE(n.contains("not-an-ip"));
}

TEST(IpNetworks, CidrBoundaries) {
  const auto n = nets({"10.0.0.0/8", "172.16.0.0/12", "fd00::/8"});
  EXPECT_TRUE(n.contains("10.0.0.0"));
  EXPECT_TRUE(n.contains("10.255.255.255"));
  EXPECT_FALSE(n.contains("11.0.0.0"));
  EXPECT_TRUE(n.contains("172.31.255.255"));  // /12: 172.16 .. 172.31
  EXPECT_FALSE(n.contains("172.32.0.0"));
  EXPECT_FALSE(n.contains("172.15.255.255"));
  EXPECT_TRUE(n.contains("fdab::1"));
  EXPECT_FALSE(n.contains("fe00::1"));
  EXPECT_FALSE(n.contains("::ffff:11.0.0.1"));
}

TEST(IpNetworks, FamiliesDoNotMix) {
  const auto everything_v4 = nets({"0.0.0.0/0"});
  EXPECT_TRUE(everything_v4.contains("203.0.113.9"));
  EXPECT_FALSE(everything_v4.contains("2001:db8::1"));
  const auto everything_v6 = nets({"::/0"});
  EXPECT_TRUE(everything_v6.contains("2001:db8::1"));
  EXPECT_FALSE(everything_v6.contains("203.0.113.9"));
}

TEST(IpNetworks, FullLengthPrefixes) {
  const auto n = nets({"192.0.2.7/32", "2001:db8::7/128"});
  EXPECT_TRUE(n.contains("192.0.2.7"));
  EXPECT_FALSE(n.contains("192.0.2.6"));
  EXPECT_TRUE(n.contains("2001:db8::7"));
  EXPECT_FALSE(n.contains("2001:db8::6"));
}

TEST(IpNetworks, MappedBlockIsIpv4) {
  const auto n = nets({"::ffff:10.0.0.0/104"});
  EXPECT_TRUE(n.contains("10.1.2.3"));
  EXPECT_FALSE(n.contains("11.1.2.3"));
}

TEST(IpNetworks, RejectsBadEntries) {
  expect_rejected("proxy.example", "not an IP");
  expect_rejected("", "not an IP");
  expect_rejected("10.0.0.0/33", "0..32");
  expect_rejected("fd00::/129", "0..128");
  expect_rejected("10.0.0.0/", "prefix");
  expect_rejected("10.0.0.0/8x", "prefix");
  expect_rejected("10.0.0.0/-1", "prefix");
  expect_rejected("10.1.2.3/8", "10.0.0.0/8");  // a typo must not trust a whole /8
  expect_rejected("fd00::1/8", "fd00::/8");
  expect_rejected("::ffff:10.0.0.0/64", "96..128");
}

TEST(ClientAddress, NoTrustedProxiesIgnoresHeader) {
  EXPECT_EQ(client_address("192.0.2.1", "203.0.113.9", IpNetworks{}), "192.0.2.1");
  EXPECT_EQ(client_address("::ffff:192.0.2.1", "", IpNetworks{}), "192.0.2.1");
}

TEST(ClientAddress, UntrustedPeerCannotSpoof) {
  const auto trusted = nets({"10.0.0.0/8"});
  EXPECT_EQ(client_address("198.51.100.4", "203.0.113.9", trusted), "198.51.100.4");
  // Claiming to come through our proxy doesn't help either.
  EXPECT_EQ(client_address("198.51.100.4", "203.0.113.9, 10.0.0.5", trusted), "198.51.100.4");
}

TEST(ClientAddress, TrustedProxyNamesTheClient) {
  const auto trusted = nets({"10.0.0.5"});
  EXPECT_EQ(client_address("10.0.0.5", "203.0.113.9", trusted), "203.0.113.9");
  EXPECT_EQ(client_address("10.0.0.5", "  203.0.113.9\t", trusted), "203.0.113.9");
  EXPECT_EQ(client_address("10.0.0.5", "2001:DB8::9", trusted), "2001:db8::9");
  EXPECT_EQ(client_address("10.0.0.5", "::ffff:203.0.113.9", trusted), "203.0.113.9");
}

TEST(ClientAddress, ClientWrittenEntriesAreIgnored) {
  // The client sent "X-Forwarded-For: 6.6.6.6"; our proxy appended the real address.
  const auto trusted = nets({"10.0.0.5"});
  EXPECT_EQ(client_address("10.0.0.5", "6.6.6.6, 203.0.113.9", trusted), "203.0.113.9");
  EXPECT_EQ(client_address("10.0.0.5", "garbage, 6.6.6.6, 203.0.113.9", trusted), "203.0.113.9");
}

TEST(ClientAddress, ChainOfTrustedProxies) {
  // client -> edge 10.0.0.1 -> inner 10.0.0.2 -> us
  const auto trusted = nets({"10.0.0.0/24"});
  EXPECT_EQ(client_address("10.0.0.2", "203.0.113.9, 10.0.0.1", trusted), "203.0.113.9");
  EXPECT_EQ(client_address("10.0.0.2", "6.6.6.6, 203.0.113.9, 10.0.0.1", trusted), "203.0.113.9");
}

TEST(ClientAddress, AllHopsTrustedGivesTheInnermost) {
  const auto trusted = nets({"10.0.0.0/24"});
  EXPECT_EQ(client_address("10.0.0.2", "10.0.0.9, 10.0.0.1", trusted), "10.0.0.9");
}

TEST(ClientAddress, MissingOrEmptyHeaderIsTheProxyItself) {
  const auto trusted = nets({"10.0.0.5"});
  for (const auto* header : {"", " ", ",", " , ,\t"}) {
    EXPECT_EQ(client_address("10.0.0.5", header, trusted), "10.0.0.5") << '"' << header << '"';
  }
}

TEST(ClientAddress, MalformedEntryStopsAtTheLastTrustedHop) {
  const auto trusted = nets({"10.0.0.0/24"});
  EXPECT_EQ(client_address("10.0.0.2", "unknown", trusted), "10.0.0.2");
  EXPECT_EQ(client_address("10.0.0.2", "203.0.113.9:4711", trusted), "10.0.0.2");
  EXPECT_EQ(client_address("10.0.0.2", "203.0.113.9, junk, 10.0.0.1", trusted), "10.0.0.1");
}

TEST(ClientAddress, TooManyHopsAreIgnored) {
  const auto trusted = nets({"10.0.0.5"});
  std::string header;
  for (std::size_t i = 0; i < kMaxForwardedHops; ++i) {
    header += (i == 0 ? "" : ",") + std::string{"203.0.113.9"};
  }
  EXPECT_EQ(client_address("10.0.0.5", header, trusted), "203.0.113.9");
  header += ",203.0.113.10";
  EXPECT_EQ(client_address("10.0.0.5", header, trusted), "10.0.0.5");
}
