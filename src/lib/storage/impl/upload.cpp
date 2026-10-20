#include "storage/impl/io.hpp"

#include <format>
#include <system_error>
#include <utility>

namespace confide::storage {

namespace fs = std::filesystem;

using common::ByteSpan;
using common::ErrCode;
using common::RelativePath;
using common::Sha256;
using common::Sha256Digest;
using detail::fail;
using detail::io_error;
using detail::last_errno;
using detail::sync_file;

namespace {

std::unexpected<common::Error> finished() {
  return fail(ErrCode::internal, "storage: upload already finished");
}

}  // namespace

Upload::Upload(detail::FilePtr file, Sha256 hasher, fs::path tmp_path, fs::path final_path,
               RelativePath path)
    : file_(std::move(file))
    , hasher_(std::move(hasher))
    , tmp_path_(std::move(tmp_path))
    , final_path_(std::move(final_path))
    , path_(std::move(path)) {}

Upload::Upload(Upload&& other) noexcept
    : file_(std::move(other.file_))
    , hasher_(std::move(other.hasher_))
    , tmp_path_(std::move(other.tmp_path_))
    , final_path_(std::move(other.final_path_))
    , path_(std::move(other.path_))
    , size_(other.size_)
    , done_(std::exchange(other.done_, true)) {}

Upload& Upload::operator=(Upload&& other) noexcept {
  if (this != &other) {
    if (!done_) {
      discard();
    }
    file_ = std::move(other.file_);
    hasher_ = std::move(other.hasher_);
    tmp_path_ = std::move(other.tmp_path_);
    final_path_ = std::move(other.final_path_);
    path_ = std::move(other.path_);
    size_ = other.size_;
    done_ = std::exchange(other.done_, true);
  }
  return *this;
}

Upload::~Upload() {
  if (!done_) {
    discard();
  }
}

void Upload::discard() noexcept {
  done_ = true;
  file_.reset();
  std::error_code ec;
  fs::remove(tmp_path_, ec);
}

Result<void> Upload::write(ByteSpan chunk) {
  if (done_) {
    return finished();
  }
  if (chunk.empty()) {
    return {};
  }
  if (std::fwrite(chunk.data(), 1, chunk.size(), file_.get()) != chunk.size()) {
    const auto ec = last_errno();
    discard();
    return io_error("write", path_.str(), ec);
  }
  if (auto r = hasher_.update(chunk); !r) {
    discard();
    return std::unexpected(std::move(r).error());
  }
  size_ += chunk.size();
  return {};
}

Result<void> Upload::write(std::string_view chunk) {
  return write(std::as_bytes(std::span{chunk.data(), chunk.size()}));
}

Result<StoredFile> Upload::commit(const std::optional<Sha256Digest>& expected) {
  if (done_) {
    return finished();
  }
  auto result = finish(expected);
  if (!result) {
    discard();
  }
  return result;
}

Result<StoredFile> Upload::finish(const std::optional<Sha256Digest>& expected) {
  CFD_TRYV(sync_file(file_.get(), path_.str()));
  if (std::fclose(file_.release()) != 0) {  // NOLINT(cppcoreguidelines-owning-memory)
    return io_error("close", path_.str(), last_errno());
  }

  CFD_TRY(digest, hasher_.finish());
  if (expected && *expected != digest) {
    return fail(ErrCode::integrity,
                std::format("storage: checksum mismatch for '{}': expected {}, got {}",
                            path_.str(), expected->to_hex(), digest.to_hex()));
  }

  // "a/b" can't be stored when "a" is a file, and "a" can't replace a
  // directory "a/": both are a clash inside the transaction, not a disk fault.
  std::error_code ec;
  fs::create_directories(final_path_.parent_path(), ec);
  if (ec == std::errc::file_exists || ec == std::errc::not_a_directory) {
    return fail(ErrCode::conflict,
                std::format("storage: a parent of '{}' is a file", path_.str()));
  }
  if (ec) {
    return io_error("create directories for", path_.str(), ec);
  }
  if (fs::is_directory(final_path_, ec)) {
    return fail(ErrCode::conflict, std::format("storage: '{}' is a directory", path_.str()));
  }

  fs::rename(tmp_path_, final_path_, ec);
  if (ec) {
    return io_error("rename", path_.str(), ec);
  }
  done_ = true;
  return StoredFile{.path=path_, .size=size_, .sha256=digest};
}

}  // namespace confide::storage
