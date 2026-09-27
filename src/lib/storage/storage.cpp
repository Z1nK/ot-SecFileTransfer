#include "storage/storage.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <format>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace confide::storage {

namespace fs = std::filesystem;

using common::ByteSpan;
using common::ErrCode;
using common::Error;
using common::RelativePath;
using common::Sha256;
using common::Sha256Digest;
using common::TransactionId;

namespace {

constexpr std::string_view kTmpDir = "tmp";
constexpr std::string_view kTransactionsDir = "transactions";
constexpr std::string_view kFilesDir = "files";

// Error details name only the relative path or transaction id, never the
// absolute storage root: `api` may pass the detail on to the client.
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

std::unexpected<Error> finished() {
  return fail(ErrCode::internal, "storage: upload already finished");
}

// Unique temp file name within this process. tmp/ is cleared on startup and
// one storage dir belongs to one server, so a counter is enough.
std::string next_tmp_name(const TransactionId& id) {
  static std::atomic<std::uint64_t> counter{0};
  return std::format("{}-{}.part", id.str(), counter.fetch_add(1, std::memory_order_relaxed));
}

Result<detail::FilePtr> open_file(const fs::path& p, const char* mode, std::string_view subject) {
  detail::FilePtr f{std::fopen(p.string().c_str(), mode)};  // NOLINT(cppcoreguidelines-owning-memory)
  if (!f) {
    const auto ec = last_errno();
    if (ec == std::errc::no_such_file_or_directory) {
      return fail(ErrCode::notfound, std::format("storage: no such file '{}'", subject));
    }
    return io_error("open", subject, ec);
  }
  return f;
}

// Pushes buffered data to the OS and then to disk, so the rename that follows
// never publishes a file whose content is still only in memory.
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

bool is_regular(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}

}  // namespace

std::string_view file_name(Document doc) {
  switch (doc) {
  case Document::meta:
    return "meta.json";
  case Document::manifest:
    return "manifest.json";
  }
  return "unknown";
}

void detail::FileCloser::operator()(std::FILE* f) const noexcept {
  std::fclose(f);  // NOLINT(cppcoreguidelines-owning-memory)
}

// --- Upload -----------------------------------------------------------------

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
  return StoredFile{path_, size_, digest};
}

// --- Download ---------------------------------------------------------------

Result<std::size_t> Download::read(std::span<std::byte> buf) {
  if (!file_) {
    return fail(ErrCode::internal, "storage: read after move");
  }
  const std::size_t n = std::fread(buf.data(), 1, buf.size(), file_.get());
  if (n < buf.size() && std::ferror(file_.get()) != 0) {
    return io_error("read", "download", last_errno());
  }
  return n;
}

// --- FileStore --------------------------------------------------------------

Result<FileStore> FileStore::open(fs::path root) {
  std::error_code ec;
  fs::create_directories(root / kTransactionsDir, ec);
  if (ec) {
    return io_error("create", kTransactionsDir, ec);
  }
  // Anything in tmp/ is an upload interrupted by the previous shutdown.
  fs::remove_all(root / kTmpDir, ec);
  if (ec) {
    return io_error("clean", kTmpDir, ec);
  }
  fs::create_directories(root / kTmpDir, ec);
  if (ec) {
    return io_error("create", kTmpDir, ec);
  }
  return FileStore{std::move(root)};
}

fs::path FileStore::tmp_dir() const {
  return root_ / kTmpDir;
}

// TransactionId is a validated UUID, so it is safe as a folder name.
fs::path FileStore::transaction_dir(const TransactionId& id) const {
  return root_ / kTransactionsDir / id.str();
}

fs::path FileStore::files_dir(const TransactionId& id) const {
  return transaction_dir(id) / kFilesDir;
}

Result<void> FileStore::create_transaction(const TransactionId& id) {
  std::error_code ec;
  const bool created = fs::create_directory(transaction_dir(id), ec);
  if (ec) {
    return io_error("create transaction", id.str(), ec);
  }
  if (!created) {
    return fail(ErrCode::conflict, std::format("storage: transaction {} exists", id.str()));
  }
  fs::create_directory(files_dir(id), ec);
  if (ec) {
    return io_error("create transaction", id.str(), ec);
  }
  return {};
}

Result<void> FileStore::remove_transaction(const TransactionId& id) {
  std::error_code ec;
  const auto removed = fs::remove_all(transaction_dir(id), ec);
  if (ec) {
    return io_error("remove transaction", id.str(), ec);
  }
  if (removed == 0) {
    return fail(ErrCode::notfound, std::format("storage: no transaction {}", id.str()));
  }
  return {};
}

bool FileStore::has_transaction(const TransactionId& id) const {
  std::error_code ec;
  return fs::is_directory(files_dir(id), ec);
}

Result<std::vector<TransactionId>> FileStore::list_transactions() const {
  std::vector<TransactionId> ids;
  std::error_code ec;
  for (fs::directory_iterator it{root_ / kTransactionsDir, ec}, end; !ec && it != end;
       it.increment(ec)) {
    if (!it->is_directory(ec)) {
      continue;
    }
    if (auto id = TransactionId::parse(it->path().filename().string())) {
      ids.push_back(std::move(*id));
    }
  }
  if (ec) {
    return io_error("list", kTransactionsDir, ec);
  }
  std::ranges::sort(ids);
  return ids;
}

Result<Upload> FileStore::begin_write(const TransactionId& id, fs::path final_path,
                                      RelativePath path) {
  if (!has_transaction(id)) {
    return fail(ErrCode::notfound, std::format("storage: no transaction {}", id.str()));
  }
  auto tmp_path = tmp_dir() / next_tmp_name(id);
  CFD_TRY(file, open_file(tmp_path, "wb", path.str()));
  CFD_TRY(hasher, Sha256::create());
  return Upload{std::move(file), std::move(hasher), std::move(tmp_path), std::move(final_path),
                std::move(path)};
}

Result<Upload> FileStore::begin_upload(const TransactionId& id, const RelativePath& path) {
  return begin_write(id, path.under(files_dir(id)), path);
}

Result<Download> FileStore::open_read(const TransactionId& id, const RelativePath& path) const {
  CFD_TRY(info, stat(id, path));
  CFD_TRY(file, open_file(path.under(files_dir(id)), "rb", path.str()));
  return Download{std::move(file), info.size};
}

Result<FileInfo> FileStore::stat(const TransactionId& id, const RelativePath& path) const {
  const auto p = path.under(files_dir(id));
  std::error_code ec;
  if (!fs::is_regular_file(p, ec)) {
    return fail(ErrCode::notfound, std::format("storage: no such file '{}'", path.str()));
  }
  const auto size = fs::file_size(p, ec);
  if (ec) {
    return io_error("stat", path.str(), ec);
  }
  return FileInfo{path, size};
}

Result<StoredFile> FileStore::hash_file(const TransactionId& id, const RelativePath& path) const {
  CFD_TRY(download, open_read(id, path));
  CFD_TRY(hasher, Sha256::create());
  std::vector<std::byte> buf(kChunkBytes);
  std::uint64_t size = 0;
  while (true) {
    CFD_TRY(n, download.read(buf));
    if (n == 0) {
      break;
    }
    CFD_TRYV(hasher.update(ByteSpan{buf.data(), n}));
    size += n;
  }
  CFD_TRY(digest, hasher.finish());
  return StoredFile{path, size, digest};
}

Result<void> FileStore::remove_file(const TransactionId& id, const RelativePath& path) {
  const auto files = files_dir(id);
  const auto p = path.under(files);
  if (!is_regular(p)) {
    return fail(ErrCode::notfound, std::format("storage: no such file '{}'", path.str()));
  }
  std::error_code ec;
  fs::remove(p, ec);
  if (ec) {
    return io_error("remove", path.str(), ec);
  }
  // Drop folders emptied by the removal; stop at files/ itself. Best effort:
  // a leftover empty folder is harmless and list_files() ignores it.
  for (auto dir = p.parent_path(); dir != files && dir.native().size() > files.native().size();
       dir = dir.parent_path()) {
    if (!fs::is_empty(dir, ec) || ec) {
      break;
    }
    fs::remove(dir, ec);
    if (ec) {
      break;
    }
  }
  return {};
}

Result<std::vector<FileInfo>> FileStore::list_files(const TransactionId& id) const {
  const auto files = files_dir(id);
  if (!has_transaction(id)) {
    return fail(ErrCode::notfound, std::format("storage: no transaction {}", id.str()));
  }
  std::vector<FileInfo> out;
  std::error_code ec;
  for (fs::recursive_directory_iterator it{files, ec}, end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) {
      continue;
    }
    const auto size = it->file_size(ec);
    if (ec) {
      break;
    }
    CFD_TRY(rel, RelativePath::from_local(files, it->path()));
    out.push_back(FileInfo{std::move(rel), size});
  }
  if (ec) {
    return io_error("list files of", id.str(), ec);
  }
  std::ranges::sort(out, {}, &FileInfo::path);
  return out;
}

Result<void> FileStore::write_document(const TransactionId& id, Document doc,
                                       std::string_view content) {
  const auto name = file_name(doc);
  CFD_TRY(rel, RelativePath::parse(name));
  CFD_TRY(upload, begin_write(id, transaction_dir(id) / name, std::move(rel)));
  CFD_TRYV(upload.write(content));
  if (auto r = upload.commit(); !r) {
    return std::unexpected(std::move(r).error());
  }
  return {};
}

Result<std::string> FileStore::read_document(const TransactionId& id, Document doc) const {
  const auto name = file_name(doc);
  CFD_TRY(file, open_file(transaction_dir(id) / name, "rb", name));
  std::string out;
  char buf[4096];  // NOLINT(cppcoreguidelines-avoid-c-arrays)
  while (true) {
    const std::size_t n = std::fread(buf, 1, sizeof(buf), file.get());
    out.append(buf, n);
    if (n < sizeof(buf)) {
      if (std::ferror(file.get()) != 0) {
        return io_error("read", name, last_errno());
      }
      break;
    }
  }
  return out;
}

}  // namespace confide::storage
