#pragma once

#include "common/id.hpp"
#include "common/time.hpp"
#include "logging/sink.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace confide::logging {

// What happened to a transaction only successful actions:
enum class BizEventType : std::uint8_t {
  tx_created,
  file_added,
  file_removed,
  committed,
  forwarded,   // pushed to a peer (sender side)
  received,    // stored from a peer (receiver side)
  downloaded,  // a whole file was streamed to the target user
  expired,     // deleted by the retention job
};

std::string_view to_string(BizEventType type);

// One business event as plain data, so `logging` does not depend on the
// `transaction` module. Fill it with designated initializers:
//
//   blog.record({.type = BizEventType::file_added, .tx = id, .actor = "alice",
//                .file = "report/a.csv", .size = 10240, .sha256 = hex});
//
// `type` and `tx` are required; everything else is optional. Empty strings
// and nullopt fields are left out of the JSON line.
struct BizEvent {
  BizEventType type;
  common::TransactionId tx;
  std::string actor{};                // user name, "peer:<server>" or "system"
  std::string from_user{};            // sender of the transaction
  std::string to_user{};              // target user
  std::string to_server{};            // target server; empty = local delivery
  std::optional<std::string> file{};  // relative path, for file events
  std::optional<uint64_t> size{};
  std::optional<std::string> sha256{};
  std::optional<uint32_t> file_count{};  // committed / forwarded / received
  std::optional<uint64_t> total_bytes{};
};

// Audit trail of who sent what to whom. Unlike TechLog it has no levels
// and never drops a record: each event is formatted and written to the sink
// synchronously, under a lock, before record() returns.
//
// One JSON object per line, schema version in "v":
//   {"v":1,"ts":"2026-09-27T12:00:03Z","event":"file_added","tx":"...",
//    "actor":"alice","file":"report/a.csv","size":10240,"sha256":"..."}
//
class BusinessLog {
public:
  static constexpr int kSchemaVersion = 1;

  BusinessLog(std::unique_ptr<ILogSink> sink, const common::IClock& clock);
  
  void record(const BizEvent& event);

  static std::string format(const BizEvent& event, common::Timestamp ts);

private:
  std::unique_ptr<ILogSink> sink_;
  const common::IClock& clock_;
  std::mutex mu_;
};

}  // namespace confide::logging
