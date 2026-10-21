#pragma once
// Local files: picking what to send, checksums.
#include "cli.hpp"

#include <common/path_sanitizer.hpp>
#include <common/sha256.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace confide::cli {

Result<common::Sha256Digest> hash_file(const std::filesystem::path& path);

struct LocalFile {
  std::filesystem::path source;
  common::RelativePath path;  // as sent to the server
};

// Files to send, sorted by path. A folder keeps its own name as the first
// path component ("report/a.csv"), a single file is sent under its file name.
// `validation` if nothing is left or two files would get the same path.
Result<std::vector<LocalFile>> collect_files(const std::vector<std::string>& args);

}  // namespace confide::cli
