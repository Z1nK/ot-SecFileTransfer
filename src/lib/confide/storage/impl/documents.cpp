#include "storage/impl/io.hpp"

#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>

namespace confide::storage {

using common::RelativePath;
using common::TransactionId;
using detail::io_error;
using detail::last_errno;
using detail::open_file;

std::string_view file_name(Document doc) {
  switch (doc) {
  case Document::meta:
    return "meta.json";
  case Document::manifest:
    return "manifest.json";
  }
  return "unknown";
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
