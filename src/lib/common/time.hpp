#pragma once
#include "common/error.hpp"

#include <chrono>
#include <string>
#include <string_view>

namespace confide::common {

// Wall-clock point in time, always UTC, second precision. Used for
// transaction creation/expiry (meta.json, API JSON, peer metadata) and
// business log events. Never use local time: both servers must read the
// same value the same way.
using Timestamp = std::chrono::sys_seconds;

// Upper bound for retention_days, so expiry arithmetic can't overflow and a
// typo like 300000 is rejected instead of keeping data for centuries.
inline constexpr int kMaxRetentionDays = 3650;

// Canonical text form "YYYY-MM-DDTHH:MM:SSZ", e.g. "2026-09-24T12:00:00Z".
std::string format_iso8601(Timestamp t);

// Accepts only the exact form format_iso8601 produces.
Result<Timestamp> parse_iso8601(std::string_view text);

// created + retention_days. `validation` unless 1 <= days <= kMaxRetentionDays.
Result<Timestamp> expires_at(Timestamp created, int retention_days);

// True once `now` has reached `expiry`.
inline bool is_expired(Timestamp expiry, Timestamp now) {
  return now >= expiry;
}

// main source of now()
struct IClock {
  virtual Timestamp now() const = 0;
  virtual ~IClock() = default;
};

class SystemClock final : public IClock {
public:
  Timestamp now() const override;
};

}  // namespace confide::common
