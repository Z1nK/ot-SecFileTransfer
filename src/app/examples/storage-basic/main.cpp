// Usage example for storage/storage.hpp: open a FileStore, create a
// transaction, upload a file in chunks, read it back, write a document,
// list and clean up.
//
//   example-storage-basic              # uses a fresh dir under the system temp dir
//   example-storage-basic path/to/root

#include <storage/storage.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

using confide::common::RelativePath;
using confide::common::Result;
using confide::common::Sha256;
using confide::common::TransactionId;
using confide::common::to_string;
using confide::storage::Document;
using confide::storage::FileStore;
using confide::storage::StoredFile;

namespace fs = std::filesystem;

namespace {

// Streams `content` into the store in small chunks, as the REST handler does
// with a request body, and checks it against the client's checksum.
Result<StoredFile> upload(FileStore& store, const TransactionId& id, std::string_view path,
                          std::string_view content) {
  CFD_TRY(rel, RelativePath::parse(path));
  CFD_TRY(expected, Sha256::of(content));

  CFD_TRY(up, store.begin_upload(id, rel));
  constexpr std::size_t kChunk = 4;
  for (std::size_t off = 0; off < content.size(); off += kChunk) {
    CFD_TRYV(up.write(content.substr(off, kChunk)));
  }
  // Nothing is visible in the transaction until commit() renames it in.
  return up.commit(expected);
}

Result<std::string> download(const FileStore& store, const TransactionId& id,
                             std::string_view path) {
  CFD_TRY(rel, RelativePath::parse(path));
  CFD_TRY(dl, store.open_read(id, rel));

  std::string out;
  std::array<std::byte, 8> buf{};
  for (;;) {
    CFD_TRY(n, dl.read(buf));
    if (n == 0) {
      break;
    }
    out.append(reinterpret_cast<const char*>(buf.data()), n);  // NOLINT
  }
  return out;
}

Result<void> run(const fs::path& root) {
  CFD_TRY(store, FileStore::open(root));
  std::cout << "store root : " << store.root().string() << '\n';

  // Transaction
  const auto id = TransactionId::generate();
  CFD_TRYV(store.create_transaction(id));
  std::cout << "created    : " << id.str() << '\n';

  // Upload + metadata document
  CFD_TRY(a, upload(store, id, "hello.txt", "Hello, confide!\n"));
  CFD_TRY(b, upload(store, id, "docs/readme.md", "# nested path\n"));
  std::cout << "uploaded   : " << a.path.str() << " (" << a.size << " B, " << a.sha256.to_hex()
            << ")\n";
  std::cout << "uploaded   : " << b.path.str() << " (" << b.size << " B)\n";

  CFD_TRYV(store.write_document(id, Document::meta, R"({"sender":"alice@siteA"})"));
  CFD_TRY(meta, store.read_document(id, Document::meta));
  std::cout << file_name(Document::meta) << "  : " << meta << '\n';

  // Listing + download
  CFD_TRY(files, store.list_files(id));
  std::cout << "files (" << files.size() << "):\n";
  for (const auto& f : files) {
    std::cout << "  - " << f.path.str() << " (" << f.size << " B)\n";
  }

  CFD_TRY(text, download(store, id, "hello.txt"));
  std::cout << "downloaded : " << text;

  // A bad checksum is rejected and the file is not stored.
  CFD_TRY(rel, RelativePath::parse("bad.txt"));
  CFD_TRY(wrong, Sha256::of("something else"));
  CFD_TRY(up, store.begin_upload(id, rel));
  CFD_TRYV(up.write("payload"));
  if (auto r = up.commit(wrong); !r) {
    std::cout << "bad commit : [" << to_string(r.error().code) << "] " << r.error().detail
              << '\n';
  }

  // Cleanup
  CFD_TRYV(store.remove_transaction(id));
  CFD_TRY(ids, store.list_transactions());
  std::cout << "removed    : " << (store.has_transaction(id) ? "no" : "yes") << ", "
            << ids.size() << " transaction(s) left\n";
  return {};
}

}  // namespace

int main(int argc, char** argv) {
  const std::span args(argv, static_cast<std::size_t>(argc));
  const fs::path root = args.size() > 1 ? fs::path(args[1])
                                        : fs::temp_directory_path() / "confide-storage-example";

  if (auto r = run(root); !r) {
    std::cerr << "error: [" << to_string(r.error().code) << "] " << r.error().detail << '\n';
    return 1;
  }
  return 0;
}
