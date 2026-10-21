#include "local_files.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace confide::cli {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kHashChunkBytes = std::size_t{1} << 20;

}  // namespace

Result<common::Sha256Digest> hash_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return fail(ErrCode::io, "cannot open " + path.string());
  }
  CFD_TRY(sha, common::Sha256::create());
  std::vector<char> buf(kHashChunkBytes);
  while (in) {
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    CFD_TRYV(sha.update(std::string_view{buf.data(), static_cast<std::size_t>(in.gcount())}));
  }
  if (in.bad()) {
    return fail(ErrCode::io, "read error in " + path.string());
  }
  return sha.finish();
}

Result<std::vector<LocalFile>> collect_files(const std::vector<std::string>& args) {
  std::vector<LocalFile> out;
  std::set<std::string> seen;
  auto add = [&](const fs::path& base, const fs::path& file) -> Result<void> {
    CFD_TRY(rel, common::RelativePath::from_local(base, file));
    if (!seen.insert(rel.str()).second) {
      return fail(ErrCode::validation, "two files would be sent as " + rel.str());
    }
    out.push_back({.source = file, .path = std::move(rel)});
    return {};
  };

  for (const auto& arg : args) {
    std::error_code ec;
    auto path = fs::absolute(arg, ec).lexically_normal();
    if (!path.has_filename()) {  // "dir/"
      path = path.parent_path();
    }
    const auto base = path.parent_path();
    if (fs::is_regular_file(path, ec)) {
      CFD_TRYV(add(base, path));
    } else if (fs::is_directory(path, ec)) {
      for (fs::recursive_directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) {
          CFD_TRYV(add(base, it->path()));
        }
      }
      if (ec) {
        return fail(ErrCode::io, std::format("{}: {}", path.string(), ec.message()));
      }
    } else {
      return fail(ErrCode::validation, arg + ": no such file or folder");
    }
  }
  if (out.empty()) {
    return fail(ErrCode::validation, "nothing to send");
  }
  std::ranges::sort(out, {}, [](const LocalFile& f) { return f.path.str(); });
  return out;
}

}  // namespace confide::cli
