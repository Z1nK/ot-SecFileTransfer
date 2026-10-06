#include <common/id.hpp>

#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using confide::common::ErrCode;
using confide::common::TransactionId;

namespace {

constexpr std::string_view kValid = "550e8400-e29b-41d4-a716-446655440000";

}  // namespace

TEST(TransactionIdGenerate, ProducesCanonicalUuidV4) {
  auto id = TransactionId::generate();
  const std::string& s = id.str();

  ASSERT_EQ(s.size(), 36U);
  EXPECT_EQ(s[8], '-');
  EXPECT_EQ(s[13], '-');
  EXPECT_EQ(s[18], '-');
  EXPECT_EQ(s[23], '-');
  EXPECT_EQ(s[14], '4');
  EXPECT_NE(std::string_view{"89ab"}.find(s[19]), std::string_view::npos);
}

TEST(TransactionIdGenerate, RoundTripsThroughParse) {
  for (int i = 0; i < 100; ++i) {
    auto id = TransactionId::generate();
    auto parsed = TransactionId::parse(id.str());
    ASSERT_TRUE(parsed.has_value()) << id.str();
    EXPECT_EQ(*parsed, id);
  }
}

TEST(TransactionIdGenerate, IdsAreUnique) {
  std::unordered_set<TransactionId> seen;
  for (int i = 0; i < 10'000; ++i) {
    EXPECT_TRUE(seen.insert(TransactionId::generate()).second);
  }
}

TEST(TransactionIdGenerate, UniqueAcrossThreads) {
  constexpr int kThreads = 8;
  constexpr int kPerThread = 1'000;
  std::mutex mu;
  std::unordered_set<TransactionId> seen;

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      std::vector<TransactionId> local;
      for (int i = 0; i < kPerThread; ++i) {
        local.push_back(TransactionId::generate());
      }
      std::lock_guard lock(mu);
      seen.insert(local.begin(), local.end());
    });
  }
  for (auto& th : threads) {
    th.join();
  }
  EXPECT_EQ(seen.size(), static_cast<size_t>(kThreads * kPerThread));
}

TEST(TransactionIdParse, AcceptsCanonicalForm) {
  auto id = TransactionId::parse(kValid);
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(id->str(), kValid);
}

TEST(TransactionIdParse, AcceptsAllVariantDigits) {
  for (char v : std::string_view{"89ab"}) {
    std::string s{kValid};
    s[19] = v;
    EXPECT_TRUE(TransactionId::parse(s).has_value()) << s;
  }
}

class TransactionIdParseRejects : public ::testing::TestWithParam<std::string_view> {};

TEST_P(TransactionIdParseRejects, WithValidationError) {
  auto id = TransactionId::parse(GetParam());
  ASSERT_FALSE(id.has_value());
  EXPECT_EQ(id.error().code, ErrCode::validation);
}

INSTANTIATE_TEST_SUITE_P(
    Malformed, TransactionIdParseRejects,
    ::testing::Values(
        "",                                        // empty
        "tx-123",                                  // old example format
        "550e8400-e29b-41d4-a716-44665544000",     // too short
        "550e8400-e29b-41d4-a716-4466554400000",   // too long
        "550E8400-E29B-41D4-A716-446655440000",    // uppercase
        "{550e8400-e29b-41d4-a716-446655440000}",  // braces
        "550e8400e29b41d4a716446655440000",        // no dashes
        "550e8400-e29b-41d4-a716_446655440000",    // wrong separator
        "550e8400-e29b-11d4-a716-446655440000",    // version 1
        "550e8400-e29b-41d4-c716-446655440000",    // wrong variant
        "550e8400-e29b-41d4-a716-44665544000g",    // non-hex
        "../../../../etc/passwd/../../../../x",    // 36 chars, path traversal
        "550e8400-e29b-41d4-a716-44665544/000",    // slash inside
        std::string_view{"550e8400-e29b-41d4-a716-4466554400\0000", 36}));  // NUL byte

TEST(TransactionId, CopyAndCompare) {
  auto a = TransactionId::generate();
  auto b = a;  // NOLINT(performance-unnecessary-copy-initialization)
  EXPECT_EQ(a, b);
  EXPECT_NE(a, TransactionId::generate());
}

TEST(TransactionId, OrderingFollowsString) {
  auto lo = TransactionId::parse("00000000-0000-4000-8000-000000000000");
  auto hi = TransactionId::parse("ffffffff-ffff-4fff-bfff-ffffffffffff");
  ASSERT_TRUE(lo && hi);
  EXPECT_LT(*lo, *hi);
}

TEST(TransactionId, UsableAsHashMapKey) {
  auto id = TransactionId::generate();
  std::unordered_map<TransactionId, int> m;
  m[id] = 1;

  auto same = TransactionId::parse(id.str());
  ASSERT_TRUE(same.has_value());
  EXPECT_EQ(m.at(*same), 1);
  EXPECT_EQ(std::hash<TransactionId>{}(id), std::hash<std::string>{}(id.str()));
}
