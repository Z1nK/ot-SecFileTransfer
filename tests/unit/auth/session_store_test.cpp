#include <auth/session_store.hpp>
#include <common/time.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <set>
#include <string>

using confide::auth::Scope;
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

TEST(SessionStore, AdminAndUserTokensDoNotMix) {
  FakeClock clock;
  SessionStore store(clock, SessionOptions{});
  const auto user = store.issue("alice").value();
  const auto admin = store.issue("alice", Scope::admin).value();
  EXPECT_TRUE(user.token.starts_with("ftu_"));
  EXPECT_TRUE(admin.token.starts_with("fta_"));

  EXPECT_EQ(store.verify(user.token).value(), "alice");
  EXPECT_EQ(store.verify(admin.token, Scope::admin).value(), "alice");
  EXPECT_FALSE(store.verify(admin.token).has_value());
  EXPECT_FALSE(store.verify(user.token, Scope::admin).has_value());
}

TEST(SessionStore, AdminTokensHaveTheirOwnLifetime) {
  FakeClock clock;
  SessionStore store(clock, SessionOptions{.ttl = seconds{1000},
                                           .idle = seconds{1000},
                                           .admin_ttl = seconds{100},
                                           .admin_idle = seconds{50}});
  const auto user = store.issue("alice").value();
  const auto admin = store.issue("alice", Scope::admin).value();
  EXPECT_EQ(admin.expires_at, clock.now() + seconds{100});

  clock.advance(seconds{50});  // admin idle limit reached
  EXPECT_FALSE(store.verify(admin.token, Scope::admin).has_value());
  EXPECT_TRUE(store.verify(user.token).has_value());
}

TEST(SessionStore, RevokeUserEndsBothScopes) {
  FakeClock clock;
  SessionStore store(clock, SessionOptions{});
  const auto user = store.issue("bob").value();
  const auto admin = store.issue("bob", Scope::admin).value();
  const auto other = store.issue("alice").value();

  EXPECT_EQ(store.revoke_user("bob"), 2U);
  EXPECT_FALSE(store.verify(user.token).has_value());
  EXPECT_FALSE(store.verify(admin.token, Scope::admin).has_value());
  EXPECT_TRUE(store.verify(other.token).has_value());
  EXPECT_EQ(store.revoke_user("nobody"), 0U);
}

TEST(SessionStore, ListsLiveSessionsOldestFirst) {
  FakeClock clock;
  SessionStore store(clock, SessionOptions{.ttl = seconds{100}, .idle = seconds{100}});
  ASSERT_TRUE(store.issue("gone").has_value());
  clock.advance(seconds{60});
  ASSERT_TRUE(store.issue("alice").has_value());
  clock.advance(seconds{1});
  ASSERT_TRUE(store.issue("bob", Scope::admin).has_value());
  clock.advance(seconds{40});  // "gone" is past its ttl

  const auto list = store.list();
  ASSERT_EQ(list.size(), 2U);
  EXPECT_EQ(list[0].user, "alice");
  EXPECT_EQ(list[0].scope, Scope::user);
  EXPECT_EQ(list[1].user, "bob");
  EXPECT_EQ(list[1].scope, Scope::admin);
}
