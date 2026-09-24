#include <common/sha256.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

using confide::common::ErrCode;
using confide::common::Sha256;
using confide::common::Sha256Digest;

namespace {

// FIPS 180-2 / NIST test vectors.
constexpr std::string_view kEmpty =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr std::string_view kAbc =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
constexpr std::string_view kTwoBlock =  // "abcdbcde...nopq", 448 bits
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
constexpr std::string_view kMillionA =
    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";

std::string hex_of(std::string_view data) {
  auto d = Sha256::of(data);
  EXPECT_TRUE(d.has_value());
  return d ? d->to_hex() : std::string{};
}

}  // namespace

TEST(Sha256, KnownVectors) {
  EXPECT_EQ(hex_of(""), kEmpty);
  EXPECT_EQ(hex_of("abc"), kAbc);
  EXPECT_EQ(hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"), kTwoBlock);
}

TEST(Sha256, StreamingMatchesOneShot) {
  const std::string data(1'000'000, 'a');
  auto h = Sha256::create();
  ASSERT_TRUE(h.has_value());

  // Uneven chunk sizes cross the 64-byte block boundary in every way.
  std::size_t pos = 0;
  for (std::size_t chunk = 1; pos < data.size(); chunk = chunk % 997 + 1) {
    const std::size_t n = std::min(chunk, data.size() - pos);
    ASSERT_TRUE(h->update(std::string_view{data}.substr(pos, n)).has_value());
    pos += n;
  }
  auto d = h->finish();
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->to_hex(), kMillionA);
}

TEST(Sha256, ByteSpanAndStringOverloadsAgree) {
  const std::string s = "abc";
  auto from_bytes = Sha256::of(std::as_bytes(std::span{s}));
  auto from_text = Sha256::of(std::string_view{s});
  ASSERT_TRUE(from_bytes && from_text);
  EXPECT_EQ(*from_bytes, *from_text);
}

TEST(Sha256, EmptyUpdateChangesNothing) {
  auto h = Sha256::create();
  ASSERT_TRUE(h.has_value());
  ASSERT_TRUE(h->update(std::string_view{}).has_value());
  ASSERT_TRUE(h->update("abc").has_value());
  ASSERT_TRUE(h->update(std::string_view{}).has_value());
  auto d = h->finish();
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->to_hex(), kAbc);
}

TEST(Sha256, FinishResetsForNextStream) {
  auto h = Sha256::create();
  ASSERT_TRUE(h.has_value());
  ASSERT_TRUE(h->update("abc").has_value());
  ASSERT_TRUE(h->finish().has_value());

  auto second = h->finish();  // nothing fed since the reset
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->to_hex(), kEmpty);
}

TEST(Sha256, MovedFromReportsInternal) {
  auto h = Sha256::create();
  ASSERT_TRUE(h.has_value());
  Sha256 other = std::move(*h);
  ASSERT_TRUE(other.update("abc").has_value());

  auto r = h->update("x");  // NOLINT(bugprone-use-after-move)
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrCode::internal);
}

TEST(Sha256DigestHex, RoundTrip) {
  auto d = Sha256Digest::from_hex(kAbc);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->to_hex(), kAbc);
}

TEST(Sha256DigestHex, UppercaseEqualsLowercase) {
  std::string upper{kAbc};
  for (char& c : upper) {
    if (c >= 'a' && c <= 'f') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  auto lo = Sha256Digest::from_hex(kAbc);
  auto up = Sha256Digest::from_hex(upper);
  ASSERT_TRUE(lo && up);
  EXPECT_EQ(*lo, *up);
  EXPECT_EQ(up->to_hex(), kAbc);  // output is always lowercase
}

class Sha256DigestHexRejects : public ::testing::TestWithParam<std::string_view> {};

TEST_P(Sha256DigestHexRejects, WithValidationError) {
  auto d = Sha256Digest::from_hex(GetParam());
  ASSERT_FALSE(d.has_value());
  EXPECT_EQ(d.error().code, ErrCode::validation);
}

INSTANTIATE_TEST_SUITE_P(
    Malformed, Sha256DigestHexRejects,
    ::testing::Values(
        "",
        "ba7816bf",                                                           // too short
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015a",    // 63
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad0",  // 65
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ag",   // non-hex
        " a7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",   // space
        "0xba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015"));  // prefix

TEST(Sha256Digest, CompareAndHash) {
  auto a = Sha256Digest::from_hex(kAbc);
  auto e = Sha256Digest::from_hex(kEmpty);
  ASSERT_TRUE(a && e);
  EXPECT_NE(*a, *e);
  EXPECT_LT(*a, *e);  // 0xba < 0xe3

  std::unordered_set<Sha256Digest> set{*a, *e, *a};
  EXPECT_EQ(set.size(), 2U);
  EXPECT_TRUE(set.contains(*Sha256Digest::from_hex(kAbc)));
}
