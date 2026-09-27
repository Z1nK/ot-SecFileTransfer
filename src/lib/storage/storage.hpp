#pragma once
#include "common/error.hpp"
#include "common/id.hpp"
#include "common/path_sanitizer.hpp"
#include "common/sha256.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace confide::storage {

using common::Result;

// Suggested buffer size for streamed reads and writes.
inline constexpr std::size_t kChunkBytes = std::size_t{1} << 20;

// A payload file inside a transaction, as found on disk.
struct FileInfo {
  common::RelativePath path;
  std::uint64_t size = 0;
};

// A payload file together with the SHA-256 of its content.
struct StoredFile {
  common::RelativePath path;
  std::uint64_t size = 0;
  common::Sha256Digest sha256;
};

// Small JSON documents kept next to a transaction's files. 
enum class Document : std::uint8_t {
  meta,      // meta.json: sender, target, state, created, retention
  manifest,  // manifest.json: written on commit, file list with sha256
};

std::string_view file_name(Document doc);

namespace detail {
struct FileCloser {
  void operator()(std::FILE* f) const noexcept;
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;
}  // namespace detail

// An upload in progress: bytes go to a file in <storage_dir>/tmp/ and into a
// SHA-256 hasher. Only commit() moves the file into the transaction, with an
// atomic rename, so an interrupted upload never leaves a partial file there
// (NFR-6). Dropping an Upload without commit() deletes the temp file.
//
// Any failed write() or commit() also deletes the temp file; the Upload is
// finished after that and further calls return `internal`.
class Upload {
public:
  Upload(Upload&& other) noexcept;
  Upload& operator=(Upload&& other) noexcept;
  Upload(const Upload&) = delete;
  Upload& operator=(const Upload&) = delete;
  ~Upload();

  Result<void> write(common::ByteSpan chunk);
  Result<void> write(std::string_view chunk);

  // Bytes written so far.
  std::uint64_t size() const noexcept { return size_; }

  // Flushes and fsyncs the temp file, checks the digest against `expected`
  // (X-Checksum-SHA256, if the client sent one) and renames the file into
  // place, replacing an existing file with the same path.
  //   `integrity` - digest differs from `expected`
  //   `conflict`  - the path clashes with a directory, or a parent component
  //                 is an existing file ("a" vs "a/b")
  //   `io`        - flush, fsync, mkdir or rename failed
  Result<StoredFile> commit(const std::optional<common::Sha256Digest>& expected = std::nullopt);

private:
  friend class FileStore;

  Upload(detail::FilePtr file, common::Sha256 hasher, std::filesystem::path tmp_path,
         std::filesystem::path final_path, common::RelativePath path);

  Result<StoredFile> finish(const std::optional<common::Sha256Digest>& expected);
  void discard() noexcept;

  detail::FilePtr file_;
  common::Sha256 hasher_;
  std::filesystem::path tmp_path_;
  std::filesystem::path final_path_;
  common::RelativePath path_;
  std::uint64_t size_ = 0;
  bool done_ = false;
};

// A stored file opened for streamed reading.
class Download {
public:
  // Reads up to buf.size() bytes; returns 0 at end of file.
  Result<std::size_t> read(std::span<std::byte> buf);

  // File size at the time it was opened.
  std::uint64_t size() const noexcept { return size_; }

private:
  friend class FileStore;

  Download(detail::FilePtr file, std::uint64_t size) : file_(std::move(file)), size_(size) {}

  detail::FilePtr file_;
  std::uint64_t size_ = 0;
};

// Disk layout (Architecture §9):
//
//   <root>/
//   ├── tmp/                         uploads in progress, cleared by open()
//   └── transactions/
//       └── <transaction-id>/
//           ├── meta.json
//           ├── manifest.json
//           └── files/<relative/path>
//
class FileStore {
public:
  // Creates the layout under `root` if needed and deletes leftovers in tmp/
  // from a previous run. `io` if a directory can't be created or cleaned.
  static Result<FileStore> open(std::filesystem::path root);

  const std::filesystem::path& root() const noexcept { return root_; }

  // --- Transactions -------------------------------------------------------

  // `conflict` if the transaction folder already exists.
  Result<void> create_transaction(const common::TransactionId& id);

  // Deletes the folder with all files and documents. `notfound` if absent.
  Result<void> remove_transaction(const common::TransactionId& id);

  bool has_transaction(const common::TransactionId& id) const;

  // Ids of all transaction folders, e.g. to load the repository on startup.
  // Folders whose name is not a valid id are skipped.
  Result<std::vector<common::TransactionId>> list_transactions() const;

  // --- Payload files ------------------------------------------------------

  // `notfound` if the transaction does not exist.
  Result<Upload> begin_upload(const common::TransactionId& id, const common::RelativePath& path);

  // `notfound` if the transaction or the file does not exist.
  Result<Download> open_read(const common::TransactionId& id,
                             const common::RelativePath& path) const;

  Result<FileInfo> stat(const common::TransactionId& id, const common::RelativePath& path) const;

  // Re-reads the file and computes its SHA-256 (checksum check on commit).
  Result<StoredFile> hash_file(const common::TransactionId& id,
                               const common::RelativePath& path) const;

  // Deletes the file and any parent folders left empty. `notfound` if absent.
  Result<void> remove_file(const common::TransactionId& id, const common::RelativePath& path);

  // All payload files of a transaction, sorted by path.
  Result<std::vector<FileInfo>> list_files(const common::TransactionId& id) const;

  // --- Documents ----------------------------------------------------------

  // Replaces the document atomically (temp file + fsync + rename).
  Result<void> write_document(const common::TransactionId& id, Document doc,
                              std::string_view content);

  // `notfound` if the transaction or the document does not exist.
  Result<std::string> read_document(const common::TransactionId& id, Document doc) const;

private:
  explicit FileStore(std::filesystem::path root) : root_(std::move(root)) {}

  std::filesystem::path tmp_dir() const;
  std::filesystem::path transaction_dir(const common::TransactionId& id) const;
  std::filesystem::path files_dir(const common::TransactionId& id) const;

  Result<Upload> begin_write(const common::TransactionId& id, std::filesystem::path final_path,
                             common::RelativePath path);

  std::filesystem::path root_;
};

}  // namespace confide::storage
