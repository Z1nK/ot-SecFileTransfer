#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string_view>

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

}  // namespace confide::logging
