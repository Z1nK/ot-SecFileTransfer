#include "storage/storage.hpp"

#include "storage/impl/io.hpp"

#include <algorithm>
#include <atomic>
#include <format>
#include <system_error>

// Upload is in impl/upload.cpp, Download and the shared file helpers in
// impl/io.cpp, the meta/manifest documents in impl/documents.cpp.

namespace confide::storage {

namespace fs = std::filesystem;

using common::ByteSpan;
using common::ErrCode;
using common::RelativePath;
using common::Sha256;
using common::TransactionId;
using detail::fail;
using detail::io_error;
using detail::open_file;

namespace {

constexpr std::string_view kTmpDir = "tmp";
constexpr std::string_view kTransactionsDir = "transactions";
constexpr std::string_view kFilesDir = "files";

// Unique temp file name within this process. tmp/ is cleared on startup and
// one storage dir belongs to one server, so a counter is enough.
std::string next_tmp_name(const TransactionId& id) {
  static std::atomic<std::uint64_t> counter{0};
  return std::format("{}-{}.part", id.str(), counter.fetch_add(1, std::memory_order_relaxed));
}

bool is_regular(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}

}  // namespace

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
  return FileInfo{.path=path, .size=size};
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
  return StoredFile{.path=path, .size=size, .sha256=digest};
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
    out.push_back(FileInfo{.path=std::move(rel), .size=size});
  }
  if (ec) {
    return io_error("list files of", id.str(), ec);
  }
  std::ranges::sort(out, {}, &FileInfo::path);
  return out;
}

}  // namespace confide::storage
