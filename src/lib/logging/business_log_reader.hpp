#pragma once

#include "common/error.hpp"
#include "common/time.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace confide::logging {

struct BizLogQuery {
  common::Timestamp from;  // inclusive
  common::Timestamp to;    // exclusive
  std::size_t limit = 1000;
};

// Lines of the business log written by BusinessLog through a FileSink at
// `path` (e.g. <log.dir>/business.log), oldest first, whose "ts" falls in
// [from, to). Reads `path.1` (the previous file after a rotation) and then
// `path`, one line at a time, and stops after `limit` matches. Older
// rotations are gone, so they can't be searched. Lines without a readable
// "ts" are skipped. A missing file counts as empty; `io` if one can't be
// read.
common::Result<std::vector<std::string>> read_business_log(const std::filesystem::path& path,
                                                           const BizLogQuery& query);

}  // namespace confide::logging
