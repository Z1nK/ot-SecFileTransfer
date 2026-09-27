#include "logging/sink.hpp"

#include <fstream>
#include <iostream>
#include <utility>

namespace confide::logging {

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

}  // namespace confide::logging
