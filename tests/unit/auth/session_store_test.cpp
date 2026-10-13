#include <auth/session_store.hpp>
#include <common/time.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <set>
#include <string>

using confide::auth::SessionOptions;
using confide::auth::SessionStore;
using confide::common::ErrCode;
using confide::common::IClock;
using confide::common::Timestamp;
using std::chrono::seconds;

namespace {

class FakeClock final : public IClock {
public:
  Timestamp now() const override { return now_; }
  void advance(seconds d) { now_ += d; }

private:
  Timestamp now_ = std::chrono::sys_days{std::chrono::year{2026} / 9 / 24};
};

}  // namespace

TEST(SessionStore, IssuesDistinctTokens) {
  FakeClock clock;
  SessionStore store(clock, SessionOptions{});
  std::set<std::string> tokens;
  for (int i = 0; i < 10; ++i) {
    tokens.insert(store.issue("alice").value().token);
  }
  EXPECT_EQ(tokens.size(), 10U);
}

TEST(SessionStore, ExpiryIsTtlFromLogin) {
  FakeClock clock;
  SessionStore store(clock, SessionOptions{.ttl = seconds{100}, .idle = seconds{50}});
  const auto issued = store.issue("alice").value();
  EXPECT_EQ(issued.expires_at, clock.now() + seconds{100});
}

TEST(SessionStore, RejectsUnknownAndMangledTokens) {
  FakeClock clock;
  SessionStore store(clock, SessionOptions{});
  auto token = store.issue("alice").value().token;
  for (const std::string& bad : {std::string{}, std::string{"ftu_"}, token.substr(4), token + "0",
                                 "ftu_" + std::string(64, '0')}) {
    auto r = store.verify(bad);
    ASSERT_FALSE(r.has_value()) << bad;
    EXPECT_EQ(r.error().code, ErrCode::auth);
  }
  token.back() = token.back() == '0' ? '1' : '0';
  EXPECT_FALSE(store.verify(token).has_value());
}

TEST(SessionStore, KeepsAtMostMaxPerUser) {
  FakeClock clock;
  SessionStore store(clock, SessionOptions{.max_per_user = 3});
  const auto first = store.issue("alice").value();
  for (int i = 0; i < 3; ++i) {
    clock.advance(seconds{1});
    ASSERT_TRUE(store.issue("alice").has_value());
  }
  ASSERT_TRUE(store.issue("bob").has_value());
  EXPECT_EQ(store.size(), 4U);                          // 3 for alice, 1 for bob
  EXPECT_FALSE(store.verify(first.token).has_value());  // the oldest went
}

TEST(SessionStore, LoginDropsExpiredSessions) {
  FakeClock clock;
  SessionStore store(clock, SessionOptions{.ttl = seconds{100}, .idle = seconds{100}});
  ASSERT_TRUE(store.issue("alice").has_value());
  ASSERT_TRUE(store.issue("bob").has_value());
  clock.advance(seconds{100});
  ASSERT_TRUE(store.issue("carol").has_value());
  EXPECT_EQ(store.size(), 1U);
}
