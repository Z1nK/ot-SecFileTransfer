#include <auth/password.hpp>
#include <gtest/gtest.h>

#include <string>

using confide::auth::PasswordHash;
using confide::common::ErrCode;

namespace {

// Real hashes use kDefaultIterations; tests keep PBKDF2 fast.
constexpr std::uint32_t kFast = 1000;

}  // namespace

TEST(PasswordHash, VerifiesOnlyTheRightPassword) {
  auto hash = PasswordHash::create("correct horse", kFast);
  ASSERT_TRUE(hash.has_value());
  EXPECT_TRUE(hash->verify("correct horse"));
  EXPECT_FALSE(hash->verify("correct horsE"));
  EXPECT_FALSE(hash->verify(""));
}

TEST(PasswordHash, RoundTripsThroughText) {
  auto hash = PasswordHash::create("pw", kFast);
  ASSERT_TRUE(hash.has_value());
  const std::string text = hash->to_string();
  EXPECT_TRUE(text.starts_with("pbkdf2-sha256$1000$"));

  auto parsed = PasswordHash::parse(text);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->to_string(), text);
  EXPECT_TRUE(parsed->verify("pw"));
}

TEST(PasswordHash, SaltIsRandom) {
  auto a = PasswordHash::create("pw", kFast);
  auto b = PasswordHash::create("pw", kFast);
  ASSERT_TRUE(a && b);
  EXPECT_NE(a->to_string(), b->to_string());
}

// Python: hashlib.pbkdf2_hmac('sha256', b'password', b'salt', 1, 32)
TEST(PasswordHash, MatchesKnownPbkdf2Vector) {
  auto hash =
      PasswordHash::parse("pbkdf2-sha256$1$c2FsdA==$Eg+2z/z4syxD5yJSVsT4N6hlSMkszDVICAWYfLcL4Xs=");
  ASSERT_TRUE(hash.has_value());
  EXPECT_TRUE(hash->verify("password"));
}

TEST(PasswordHash, RejectsMalformed) {
  for (const char* bad : {
           "",
           "<hash>",
           "pbkdf2-sha1$1000$c2FsdA==$a2V5",
           "pbkdf2-sha256$1000$c2FsdA==",
           "pbkdf2-sha256$1000$c2FsdA==$a2V5$x",
           "pbkdf2-sha256$abc$c2FsdA==$a2V5",
           "pbkdf2-sha256$0$c2FsdA==$a2V5",
           "pbkdf2-sha256$99999999$c2FsdA==$a2V5",
           "pbkdf2-sha256$1000$$a2V5",
           "pbkdf2-sha256$1000$c2FsdA=$a2V5",
       }) {
    auto r = PasswordHash::parse(bad);
    ASSERT_FALSE(r.has_value()) << bad;
    EXPECT_EQ(r.error().code, ErrCode::validation) << bad;
  }
}

TEST(PasswordHash, RejectsBadIterations) {
  EXPECT_FALSE(PasswordHash::create("pw", 0).has_value());
  EXPECT_FALSE(PasswordHash::create("pw", PasswordHash::kMaxIterations + 1).has_value());
}
