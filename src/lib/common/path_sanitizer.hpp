#pragma once
#include "common/error.hpp"

#include <compare>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace confide::common {

inline constexpr std::size_t kMaxPathBytes = 4096;
inline constexpr std::size_t kMaxComponentBytes = 255;

// Relative path of a file inside a transaction, e.g. "report/img/b.png".
// Always '/'-separated. Only parse() and from_local() create one, so every
// RelativePath is safe to join under a root directory (FR-6, NFR-7).
//
// Invalid input is rejected, never "cleaned up": if "a/./b" were turned into
// "a/b", two different strings would name the same file.
class RelativePath {
public:
  // Untrusted text: URL segment (already URL-decoded exactly once by api),
  // peer push, manifest.json, file list from the server in `ftc get`.
  // `validation` on:
  //   - empty, longer than kMaxPathBytes, a component over kMaxComponentBytes
  //   - leading '/' (absolute), trailing '/', empty component ("a//b")
  //   - "." or ".." component
  //   - '\' (path escape on Windows), ':' (drive "C:", Windows streams)
  //   - NUL or other control characters
  static Result<RelativePath> parse(std::string_view raw);

  // `ftc send`: `file` found while walking `base` -> "sub/dir/file".
  // `validation` if `file` is not inside `base` or the result fails parse().
  static Result<RelativePath> from_local(const std::filesystem::path& base,
                                         const std::filesystem::path& file);

  // '/'-separated form for URLs, manifest.json and logs.
  const std::string& str() const noexcept { return value_; }

  // root / path in native form. Safe by construction: the path has no
  // absolute prefix and no "..", so the result is always inside `root`.
  std::filesystem::path under(const std::filesystem::path& root) const;

  auto operator<=>(const RelativePath&) const = default;

private:
  explicit RelativePath(std::string value) : value_(std::move(value)) {}

  std::string value_;
};

}  // namespace confide::common

// Hash specialization for RelativePath to be used in unordered containers.
template <>
struct std::hash<confide::common::RelativePath> {
  std::size_t operator()(const confide::common::RelativePath& p) const noexcept {
    return std::hash<std::string>{}(p.str());
  }
};
