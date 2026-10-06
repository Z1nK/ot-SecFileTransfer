#include "logging/tech_log.hpp"

#include "logging/json.hpp"

#include <chrono>
#include <format>
#include <functional>
#include <utility>

namespace confide::logging {

TechLog::TechLog(std::unique_ptr<ILogSink> sink, size_t capacity)
    : sink_(std::move(sink)), capacity_(capacity) {
  writer_ = std::thread([this] { run(); });
}

TechLog::~TechLog() {
  {
    std::lock_guard lock(mu_);
    stop_.store(true, std::memory_order_relaxed);
  }
  cv_.notify_all();
  if (writer_.joinable()) {
    writer_.join();
  }
}

void TechLog::push(LogLevel level, std::string_view tag, std::string message) {
  auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
  auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());

  std::string line = std::format(R"({{"ts":{},"lvl":"{}","tag":"{}","msg":"{}","tid":{}}})", ts,
                                 to_string(level), json_escape(tag), json_escape(message), tid);

  std::lock_guard lock(mu_);
  if (queue_.size() >= capacity_) {
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  queue_.push_back(Record{.level=level, .line=std::move(line)});
  cv_.notify_one();
}

void TechLog::set_level(LogLevel level) {
  level_.store(level, std::memory_order_relaxed);
}

LogLevel TechLog::level() const {
  return level_.load(std::memory_order_relaxed);
}

uint64_t TechLog::dropped() const {
  return dropped_.load(std::memory_order_relaxed);
}

void TechLog::run() {
  while (true) {
    std::unique_lock lock(mu_);
    cv_.wait(lock, [this] { return stop_.load(std::memory_order_relaxed) || !queue_.empty(); });
    if (queue_.empty() && stop_.load(std::memory_order_relaxed)) {
      return;
    }

    Record record = std::move(queue_.front());
    queue_.pop_front();
    lock.unlock();

    sink_->write(record.level, record.line);
  }
}

}  // namespace confide::logging
