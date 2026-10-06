#include <common/time.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <string_view>

using confide::common::ErrCode;
using confide::common::expires_at;
using confide::common::format_iso8601;
using confide::common::IClock;
using confide::common::is_expired;
using confide::common::kMaxRetentionDays;
using confide::common::parse_iso8601;
using confide::common::SystemClock;
using confide::common::Timestamp;

using namespace std::chrono;

namespace {

// 2026-09-24T12:34:56Z
constexpr Timestamp kT = sys_days{2026y / September / 24} + 12h + 34min + 56s;

// Manually driven clock, as services will use it in their own tests.
class FakeClock final : public IClock {
public:
  explicit FakeClock(Timestamp t) : now_(t) {}
  Timestamp now() const override { return now_; }
  void advance(seconds d) { now_ += d; }

private:
  Timestamp now_;
};

}  // namespace

TEST(FormatIso8601, CanonicalUtcForm) {
  EXPECT_EQ(format_iso8601(kT), "2026-09-24T12:34:56Z");
  EXPECT_EQ(format_iso8601(Timestamp{}), "1970-01-01T00:00:00Z");
}

TEST(ParseIso8601, AcceptsCanonicalForm) {
  auto t = parse_iso8601("2026-09-24T12:34:56Z");
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(*t, kT);
}

TEST(ParseIso8601, RoundTripsWithFormat) {
  for (Timestamp t : {Timestamp{}, kT, Timestamp{sys_days{2024y / February / 29}},
                      Timestamp{sys_days{9999y / December / 31} + 23h + 59min + 59s}}) {
    auto parsed = parse_iso8601(format_iso8601(t));
    ASSERT_TRUE(parsed.has_value()) << format_iso8601(t);
    EXPECT_EQ(*parsed, t);
  }
}

class ParseIso8601Rejects : public ::testing::TestWithParam<std::string_view> {};

TEST_P(ParseIso8601Rejects, WithValidationError) {
  auto t = parse_iso8601(GetParam());
  ASSERT_FALSE(t.has_value());
  EXPECT_EQ(t.error().code, ErrCode::validation);
}

INSTANTIATE_TEST_SUITE_P(
    Malformed, ParseIso8601Rejects,
    ::testing::Values(
        "",                           // empty
        "2026-09-24",                 // date only
        "2026-09-24T12:34:56",        // no Z: local time is ambiguous
        "2026-09-24T12:34:56+02:00",  // offsets not accepted
        "2026-09-24T12:34:56.123Z",   // fractional seconds
        "2026-09-24 12:34:56Z",       // space separator
        "2026-09-24t12:34:56z",       // lowercase
        "2026-9-24T12:34:56Z",        // unpadded month
        "2026-09-24T12:34:56Z ",      // trailing space
        "2026-13-24T12:34:56Z",       // month 13
        "2026-02-30T12:34:56Z",       // Feb 30
        "2025-02-29T12:34:56Z",       // not a leap year
        "2026-09-24T24:00:00Z",       // hour 24
        "2026-09-24T12:60:00Z",       // minute 60
        "2026-09-24T12:34:60Z",       // leap second
        "2026-00-24T12:34:56Z",       // month 0
        "2026-09-00T12:34:56Z"));     // day 0

TEST(ExpiresAt, AddsWholeDays) {
  auto e = expires_at(kT, 30);
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(format_iso8601(*e), "2026-10-24T12:34:56Z");
}

TEST(ExpiresAt, AcceptsBounds) {
  EXPECT_TRUE(expires_at(kT, 1).has_value());
  EXPECT_TRUE(expires_at(kT, kMaxRetentionDays).has_value());
}

TEST(ExpiresAt, RejectsOutOfRange) {
  for (int days : {0, -1, kMaxRetentionDays + 1, 300'000}) {
    auto e = expires_at(kT, days);
    ASSERT_FALSE(e.has_value()) << days;
    EXPECT_EQ(e.error().code, ErrCode::validation);
  }
}

TEST(IsExpired, ExpiresExactlyAtDeadline) {
  const Timestamp expiry = kT + days{1};
  EXPECT_FALSE(is_expired(expiry, expiry - 1s));
  EXPECT_TRUE(is_expired(expiry, expiry));
  EXPECT_TRUE(is_expired(expiry, expiry + 1s));
}

TEST(FakeClockUsage, DrivesExpiryWithoutWaiting) {
  FakeClock clock{kT};
  auto expiry = expires_at(clock.now(), 30);
  ASSERT_TRUE(expiry.has_value());

  clock.advance(days{30} - 1s);
  EXPECT_FALSE(is_expired(*expiry, clock.now()));
  clock.advance(1s);
  EXPECT_TRUE(is_expired(*expiry, clock.now()));
}

TEST(SystemClock, NowIsCurrentUtcSecond) {
  SystemClock clock;
  const auto before = floor<seconds>(system_clock::now());
  const Timestamp now = clock.now();
  const auto after = floor<seconds>(system_clock::now());

  EXPECT_GE(now, before);
  EXPECT_LE(now, after);
}
