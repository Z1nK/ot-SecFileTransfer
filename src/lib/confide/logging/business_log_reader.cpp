#include "logging/business_log_reader.hpp"

#include <format>
#include <fstream>
#include <optional>
#include <string_view>

namespace confide::logging {

namespace {

using common::ErrCode;
using common::Error;
using common::Timestamp;

// The value of "ts" in a line BusinessLog::format() wrote, nullopt if the
// line has none.
std::optional<Timestamp> timestamp_of(std::string_view line) {
  constexpr std::string_view kKey = R"("ts":")";
  const auto start = line.find(kKey);
  if (start == std::string_view::npos) {
    return std::nullopt;
  }
  const auto value = line.substr(start + kKey.size());
  const auto end = value.find('"');
  if (end == std::string_view::npos) {
    return std::nullopt;
  }
  auto ts = common::parse_iso8601(value.substr(0, end));
  return ts ? std::optional{*ts} : std::nullopt;
}

// Appends the matching lines of one file to `out`; false once `out` is full.
common::Result<bool> scan(const std::filesystem::path& path, const BizLogQuery& query,
                          std::vector<std::string>& out) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return true;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::unexpected(Error::make(ErrCode::io, std::format("cannot open {}", path.string())));
  }
  std::string line;
  while (std::getline(in, line)) {
    const auto ts = timestamp_of(line);
    if (!ts || *ts < query.from || *ts >= query.to) {
      continue;
    }
    out.push_back(std::move(line));
    if (out.size() >= query.limit) {
      return false;
    }
  }
  if (in.bad()) {
    return std::unexpected(Error::make(ErrCode::io, std::format("cannot read {}", path.string())));
  }
  return true;
}

}  // namespace

common::Result<std::vector<std::string>> read_business_log(const std::filesystem::path& path,
                                                           const BizLogQuery& query) {
  std::vector<std::string> out;
  if (query.limit == 0 || query.from >= query.to) {
    return out;
  }
  auto rotated = path;
  rotated += ".1";
  CFD_TRY(more, scan(rotated, query, out));
  if (more) {
    CFD_TRYV(scan(path, query, out));
  }
  return out;
}

}  // namespace confide::logging
