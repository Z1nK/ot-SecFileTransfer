#include "common/time.hpp"

#include <charconv>
#include <cstddef>
#include <format>

namespace confide::common {

namespace {

constexpr std::string_view kIsoPattern = "dddd-dd-ddTdd:dd:ddZ";

// Checks the character layout only; field ranges are checked by the caller.
bool has_iso_shape(std::string_view text) {
  if (text.size() != kIsoPattern.size()) {
    return false;
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char want = kIsoPattern[i];
    const bool ok = want == 'd' ? (text[i] >= '0' && text[i] <= '9') : text[i] == want;
    if (!ok) {
      return false;
    }
  }
  return true;
}

// Digits text[pos, pos + len) as a number; the shape is already checked.
int digits(std::string_view text, std::size_t pos, std::size_t len) {
  int v = 0;
  for (std::size_t i = pos; i < pos + len; ++i) {
    v = (v * 10) + (text[i] - '0');
  }
  return v;
}

}  // namespace

std::string format_iso8601(Timestamp t) {
  return std::format("{:%FT%TZ}", t);
}

Result<Timestamp> parse_iso8601(std::string_view text) {
  auto fail = [] {
    return std::unexpected(Error::make(ErrCode::validation, "malformed timestamp"));
  };

  if (!has_iso_shape(text)) {
    return fail();
  }
  // Parsed by hand: std::chrono::parse accepts minute and second 60.
  const std::chrono::year_month_day date{std::chrono::year{digits(text, 0, 4)},
                                         std::chrono::month{static_cast<unsigned>(digits(text, 5, 2))},
                                         std::chrono::day{static_cast<unsigned>(digits(text, 8, 2))}};
  const int hour = digits(text, 11, 2);
  const int minute = digits(text, 14, 2);
  const int second = digits(text, 17, 2);
  if (!date.ok() || hour > 23 || minute > 59 || second > 59) {
    return fail();
  }
  return std::chrono::sys_days{date} + std::chrono::hours{hour} + std::chrono::minutes{minute} +
         std::chrono::seconds{second};
}

Result<Timestamp> parse_time_arg(std::string_view text, Timestamp now) {
  using namespace std::chrono;
  if (text == "now") {
    return now;
  }
  if (text.size() == 10) {
    if (auto day = parse_iso8601(std::string{text} + "T00:00:00Z")) {
      return day;
    }
  }
  if (text.size() > 1 && text.size() <= 8) {
    std::int64_t n = 0;
    const auto* end = text.data() + text.size() - 1;
    const auto [ptr, ec] = std::from_chars(text.data(), end, n);
    if (ec == std::errc{} && ptr == end && n >= 0) {
      switch (text.back()) {
      case 's':
        return now - seconds{n};
      case 'm':
        return now - minutes{n};
      case 'h':
        return now - hours{n};
      case 'd':
        return now - days{n};
      case 'w':
        return now - weeks{n};
      default:
        break;
      }
    }
  }
  auto exact = parse_iso8601(text);
  if (!exact) {
    return std::unexpected(Error::make(
        ErrCode::validation,
        std::format("'{}': expected YYYY-MM-DD[THH:MM:SSZ], now, or an age like 2h, 7d", text)));
  }
  return exact;
}

Result<Timestamp> expires_at(Timestamp created, int retention_days) {
  if (retention_days < 1 || retention_days > kMaxRetentionDays) {
    return std::unexpected(Error::make(
        ErrCode::validation,
        std::format("retention_days must be 1..{}, got {}", kMaxRetentionDays, retention_days)));
  }
  return created + std::chrono::days{retention_days};
}

Timestamp SystemClock::now() const {
  return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
}

}  // namespace confide::common
