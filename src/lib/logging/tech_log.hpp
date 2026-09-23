#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace confide::logging {

enum class LogLevel : std::uint8_t { trace, debug, info, warn, error };

std::string_view to_string(LogLevel level);

struct ILogSink {
  virtual void write(LogLevel level, std::string_view jsonl) = 0;
  virtual ~ILogSink() = default;
};

class ConsoleSink final : public ILogSink {
public:
  void write(LogLevel level, std::string_view jsonl) override;
};

class FileSink final : public ILogSink {
public:
  FileSink(std::filesystem::path path, uint64_t rotate_bytes);
  void write(LogLevel level, std::string_view jsonl) override;

private:
  void rotate();

  std::filesystem::path path_;
  uint64_t rotate_bytes_;
  uint64_t written_ = 0;
  std::mutex mu_;
};

class TechLog {
public:
  explicit TechLog(std::unique_ptr<ILogSink> sink, size_t capacity = 64 * 1024);
  ~TechLog();

  TechLog(const TechLog&) = delete;
  TechLog& operator=(const TechLog&) = delete;

  template <class... Args>
  void log(LogLevel lvl, std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    if (lvl < level()) {
      return;
    }
    push(lvl, tag, std::format(fmt, std::forward<Args>(args)...));
  }

  template <class... Args>
  void trace(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    log(LogLevel::trace, tag, fmt, std::forward<Args>(args)...);
  }
  template <class... Args>
  void debug(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    log(LogLevel::debug, tag, fmt, std::forward<Args>(args)...);
  }
  template <class... Args>
  void info(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    log(LogLevel::info, tag, fmt, std::forward<Args>(args)...);
  }
  template <class... Args>
  void warn(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    log(LogLevel::warn, tag, fmt, std::forward<Args>(args)...);
  }
  template <class... Args>
  void error(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    log(LogLevel::error, tag, fmt, std::forward<Args>(args)...);
  }

  void set_level(LogLevel level);
  LogLevel level() const;
  uint64_t dropped() const;

private:
  void push(LogLevel level, std::string_view tag, std::string message);
  void run();

  std::unique_ptr<ILogSink> sink_;
  size_t capacity_;
  std::atomic<LogLevel> level_{LogLevel::info};
  std::atomic<uint64_t> dropped_{0};

  struct Record {
    LogLevel level;
    std::string line;
  };

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Record> queue_;
  std::atomic<bool> stop_{false};
  std::thread writer_;
};

}  // namespace confide::logging
