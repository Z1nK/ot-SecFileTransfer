#include "logging/business_log.hpp"

#include "logging/json.hpp"

#include <format>
#include <utility>

namespace confide::logging {

namespace {

// Appends `,"key":value` pieces to one JSON object.
class JsonLine {
public:
  void str(std::string_view key, std::string_view value) {
    if (!value.empty()) {
      out_ += std::format(R"(,"{}":"{}")", key, json_escape(value));
    }
  }
  void opt(std::string_view key, const std::optional<std::string>& value) {
    if (value) {
      out_ += std::format(R"(,"{}":"{}")", key, json_escape(*value));
    }
  }
  template <class T>
  void num(std::string_view key, const std::optional<T>& value) {
    if (value) {
      out_ += std::format(R"(,"{}":{})", key, *value);
    }
  }
  std::string finish(std::string head) && { return std::move(head) + out_ + '}'; }

private:
  std::string out_;
};

}  // namespace

std::string_view to_string(BizEventType type) {
  switch (type) {
  case BizEventType::tx_created:
    return "tx_created";
  case BizEventType::file_added:
    return "file_added";
  case BizEventType::file_removed:
    return "file_removed";
  case BizEventType::committed:
    return "committed";
  case BizEventType::forwarded:
    return "forwarded";
  case BizEventType::received:
    return "received";
  case BizEventType::downloaded:
    return "downloaded";
  case BizEventType::expired:
    return "expired";
  }
  return "unknown";
}

BusinessLog::BusinessLog(std::unique_ptr<ILogSink> sink, const common::IClock& clock)
    : sink_(std::move(sink)), clock_(clock) {}

std::string BusinessLog::format(const BizEvent& e, common::Timestamp ts) {
  // Fixed field order, so lines are easy to read and diff.
  JsonLine line;
  line.str("actor", e.actor);
  line.str("from", e.from_user);
  line.str("to", e.to_user);
  line.str("server", e.to_server);
  line.opt("file", e.file);
  line.num("size", e.size);
  line.opt("sha256", e.sha256);
  line.num("files", e.file_count);
  line.num("bytes", e.total_bytes);
  return std::move(line).finish(std::format(R"({{"v":{},"ts":"{}","event":"{}","tx":"{}")",
                                            kSchemaVersion, common::format_iso8601(ts),
                                            to_string(e.type), e.tx.str()));
}

void BusinessLog::record(const BizEvent& event) {
  std::string line = format(event, clock_.now());
  std::lock_guard lock(mu_);
  sink_->write(LogLevel::info, line);
}

}  // namespace confide::logging
