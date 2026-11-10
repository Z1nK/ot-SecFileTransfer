#include "config/config.hpp"

#include "config/parser.hpp"

#include <format>
#include <fstream>
#include <sstream>
#include <string>

namespace confide::config {

Result<Config> ConfigLoader::from_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::unexpected(common::Error::make(
        common::ErrCode::io, std::format("cannot open config file '{}'", path.string())));
  }
  // Relative paths in the file are resolved against the file's directory,
  // so the server behaves the same whatever its working directory is.
  std::error_code ec;
  auto base = std::filesystem::absolute(path, ec).parent_path();
  return detail::Parser::parse(in, path.string(), ec ? path.parent_path() : base);
}

Result<Config> ConfigLoader::from_string(std::string_view toml) {
  std::istringstream in{std::string(toml)};
  return detail::Parser::parse(in, "<string>", {});
}

}  // namespace confide::config
