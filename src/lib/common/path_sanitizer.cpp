#include "common/path_sanitizer.hpp"

#include <format>

namespace confide::common {

namespace {

// Error detail never echoes the raw path: it is attacker-controlled and may
// contain control characters that would end up in logs.
std::unexpected<Error> invalid(std::string_view why) {
  return std::unexpected(Error::make(ErrCode::validation, std::format("invalid path: {}", why)));
}

bool is_forbidden_char(char c) {
  const auto u = static_cast<unsigned char>(c);
  return u < 0x20 || u == 0x7f || c == '\\' || c == ':';
}

}  // namespace

Result<RelativePath> RelativePath::parse(std::string_view raw) {
  if (raw.empty()) {
    return invalid("empty");
  }
  if (raw.size() > kMaxPathBytes) {
    return invalid("too long");
  }
  if (raw.front() == '/') {
    return invalid("absolute");
  }
  for (char c : raw) {
    if (is_forbidden_char(c)) {
      return invalid("forbidden character");
    }
  }

  // Split on '/'. A trailing '/' produces a final empty component.
  std::size_t start = 0;
  while (true) {
    const std::size_t end = raw.find('/', start);
    const std::string_view part = raw.substr(start, end - start);
    if (part.empty()) {
      return invalid("empty component");
    }
    if (part == "." || part == "..") {
      return invalid("dot component");
    }
    if (part.size() > kMaxComponentBytes) {
      return invalid("component too long");
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }

  return RelativePath{std::string{raw}};
}

Result<RelativePath> RelativePath::from_local(const std::filesystem::path& base,
                                              const std::filesystem::path& file) {
  const std::filesystem::path rel = file.lexically_normal().lexically_relative(
      base.lexically_normal());
  if (rel.empty() || rel.is_absolute() || *rel.begin() == "..") {
    return invalid("outside base directory");
  }
  // generic_string() uses '/' on every platform. On POSIX a file name that
  // itself contains '\' or ':' stays as is and is rejected by parse().
  return parse(rel.generic_string());
}

std::filesystem::path RelativePath::under(const std::filesystem::path& root) const {
  return root / std::filesystem::path{value_};
}

}  // namespace confide::common
