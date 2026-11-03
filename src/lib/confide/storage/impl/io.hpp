#pragma once
// Internal to storage: error helpers and stdio file handling shared by
// FileStore, Upload and Download.
#include "storage/storage.hpp"

#include <cstdio>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace confide::storage::detail {

// Error details name only the relative path or transaction id, never the
// absolute storage root: `api` may pass the detail on to the client.
std::unexpected<common::Error> fail(common::ErrCode code, std::string detail);

std::unexpected<common::Error> io_error(std::string_view what, std::string_view subject,
                                        std::error_code ec);

std::error_code last_errno();

// `notfound` if the file does not exist, `io` for any other failure.
Result<FilePtr> open_file(const std::filesystem::path& p, const char* mode,
                          std::string_view subject);

// Pushes buffered data to the OS and then to disk, so the rename that follows
// never publishes a file whose content is still only in memory.
Result<void> sync_file(std::FILE* f, std::string_view subject);

}  // namespace confide::storage::detail
