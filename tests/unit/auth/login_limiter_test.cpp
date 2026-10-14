#include <auth/login_limiter.hpp>
#include <common/time.hpp>
#include <gtest/gtest.h>

#include <chrono>

using confide::auth::LoginLimiter;
using confide::auth::LoginLimiterOptions;
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

constexpr LoginLimiterOptions kOptions{.free_per_pair = 2,
                                       .free_per_source = 5,
                                       .free_per_user = 8,
                                       .base_delay = seconds{10},
                                       .max_delay = seconds{40}};

bool throttled(LoginLimiter& l, const char* user, const char* source) {
  auto r = l.check(user, source);
  return !r && r.error().code == ErrCode::throttled;
}

}  // namespace

TEST(LoginLimiter, BackoffDoublesUpToTheMaximum) {
  FakeClock clock;
  LoginLimiter l(clock, kOptions);
  l.failed("bob", "a");
  l.failed("bob", "a");
  EXPECT_FALSE(throttled(l, "bob", "a"));

  const seconds expected[] = {seconds{10}, seconds{20}, seconds{40}, seconds{40}};
  for (const auto wait : expected) {
    l.failed("bob", "a");
    EXPECT_TRUE(throttled(l, "bob", "a"));
    clock.advance(wait - seconds{1});
    EXPECT_TRUE(throttled(l, "bob", "a")) << wait.count();
    clock.advance(seconds{1});
    EXPECT_FALSE(throttled(l, "bob", "a")) << wait.count();
  }
}

TEST(LoginLimiter, UserNamesIgnoreCase) {
  FakeClock clock;
  LoginLimiter l(clock, kOptions);
  for (int i = 0; i < 3; ++i) {
    l.failed("Bob", "a");
  }
  EXPECT_TRUE(throttled(l, "bob", "a"));
}

TEST(LoginLimiter, OneAddressGuessingManyNamesIsThrottled) {
  FakeClock clock;
  LoginLimiter l(clock, kOptions);
  const char* names[] = {"u1", "u2", "u3", "u4", "u5", "u6"};
  for (const char* n : names) {
    l.failed(n, "a");
  }
  EXPECT_TRUE(throttled(l, "someone-else", "a"));
  EXPECT_FALSE(throttled(l, "someone-else", "b"));
}

TEST(LoginLimiter, SuccessClearsTheUserButNotTheAddress) {
  FakeClock clock;
  LoginLimiter l(clock, kOptions);
  for (int i = 0; i < 2; ++i) {
    l.failed("bob", "a");
  }
  l.succeeded("bob", "a");
  for (int i = 0; i < 2; ++i) {
    l.failed("bob", "a");
  }
  EXPECT_FALSE(throttled(l, "bob", "a"));  // pair count restarted at 0

  for (int i = 0; i < 2; ++i) {
    l.failed("carol", "a");  // address total is now 6 > 5
  }
  EXPECT_TRUE(throttled(l, "dave", "a"));
}
