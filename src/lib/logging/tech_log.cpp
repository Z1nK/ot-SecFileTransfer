#include "logging/tech_log.hpp"

#include <chrono>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <utility>

namespace confide::logging {

namespace {
std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
      } else {
        out += c;
      }
    }
  }
  return out;
}
}  // namespace

std::string_view to_string(LogLevel level) {
  switch (level) {
  case LogLevel::trace:
    return "trace";
  case LogLevel::debug:
    return "debug";
  case LogLevel::info:
    return "info";
  case LogLevel::warn:
    return "warn";
  case LogLevel::error:
    return "error";
  }
  return "unknown";
}

void ConsoleSink::write(LogLevel /*level*/, std::string_view jsonl) {
  std::cout << jsonl << '\n';
}

FileSink::FileSink(std::filesystem::path path, uint64_t rotate_bytes)
    : path_(std::move(path)), rotate_bytes_(rotate_bytes) {
  std::error_code ec;
  if (std::filesystem::exists(path_, ec)) {
    written_ = std::filesystem::file_size(path_, ec);
  }
}

void FileSink::rotate() {
  std::error_code ec;
  auto rotated = path_;
  rotated += ".1";
  std::filesystem::rename(path_, rotated, ec);
  written_ = 0;
}

void FileSink::write(LogLevel level, std::string_view jsonl) {
  std::lock_guard lock(mu_);
  if (rotate_bytes_ > 0 && written_ + jsonl.size() > rotate_bytes_) {
    rotate();
  }

  std::ofstream out(path_, std::ios::app | std::ios::binary);
  out << jsonl << '\n';
  written_ += jsonl.size() + 1;
  if (level == LogLevel::error) {
    out.flush();
  }
}

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
  queue_.push_back(Record{level, std::move(line)});
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
