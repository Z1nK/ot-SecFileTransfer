#include "storage/impl/io.hpp"

#include <cerrno>
#include <format>
#include <utility>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace confide::storage {

namespace fs = std::filesystem;

using common::ErrCode;
using common::Error;

namespace detail {

std::unexpected<Error> fail(ErrCode code, std::string detail) {
  return std::unexpected(Error::make(code, std::move(detail)));
}

std::unexpected<Error> io_error(std::string_view what, std::string_view subject,
                                std::error_code ec) {
  return fail(ErrCode::io, std::format("storage: {} '{}': {}", what, subject, ec.message()));
}

std::error_code last_errno() {
  return {errno, std::generic_category()};
}

Result<FilePtr> open_file(const fs::path& p, const char* mode, std::string_view subject) {
  FilePtr f{std::fopen(p.string().c_str(), mode)};  // NOLINT(cppcoreguidelines-owning-memory)
  if (!f) {
    const auto ec = last_errno();
    if (ec == std::errc::no_such_file_or_directory) {
      return fail(ErrCode::notfound, std::format("storage: no such file '{}'", subject));
    }
    return io_error("open", subject, ec);
  }
  return f;
}

Result<void> sync_file(std::FILE* f, std::string_view subject) {
  if (std::fflush(f) != 0) {
    return io_error("flush", subject, last_errno());
  }
#ifdef _WIN32
  if (::_commit(::_fileno(f)) != 0) {
#else
  if (::fsync(::fileno(f)) != 0) {
#endif
    return io_error("fsync", subject, last_errno());
  }
  return {};
}

void FileCloser::operator()(std::FILE* f) const noexcept {
  std::fclose(f);  // NOLINT(cppcoreguidelines-owning-memory)
}

}  // namespace detail

using detail::io_error;
using detail::last_errno;

// --- Download ---------------------------------------------------------------

Result<std::size_t> Download::read(std::span<std::byte> buf) {
  if (!file_) {
    return detail::fail(ErrCode::internal, "storage: read after move");
  }
  const std::size_t n = std::fread(buf.data(), 1, buf.size(), file_.get());
  if (n < buf.size() && std::ferror(file_.get()) != 0) {
    return io_error("read", "download", last_errno());
  }
  return n;
}

}  // namespace confide::storage
