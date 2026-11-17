#include "settings.hpp"

#include <toml.hpp>

#include <array>
#include <cstdlib>
#include <format>
#include <fstream>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef FTC_SYSTEM_CONFIG
#define FTC_SYSTEM_CONFIG "/etc/ftc/ftc.toml"
#endif

namespace confide::client {

namespace fs = std::filesystem;

namespace {

std::unexpected<Error> invalid(const std::string& source, std::string_view what) {
  return fail(ErrCode::validation, std::format("{}: {}", source, what));
}

Result<std::optional<std::string>> string_key(const toml::value& root, const std::string& key,
                                              const std::string& source) {
  if (!root.contains(key)) {
    return std::nullopt;
  }
  const auto& v = root.at(key);
  if (!v.is_string()) {
    return invalid(source, std::format("{}: expected a string", key));
  }
  return std::optional<std::string>{v.as_string()};
}

#ifdef _WIN32
struct Freer {
  void operator()(void* p) const noexcept { std::free(p); }  // NOLINT(cppcoreguidelines-no-malloc)
};

std::optional<fs::path> env_dir(const wchar_t* name) {
  wchar_t* value = nullptr;
  std::size_t len = 0;
  if (::_wdupenv_s(&value, &len, name) == 0 && value != nullptr) {
    const std::unique_ptr<wchar_t, Freer> owned{value};
    fs::path dir{value};
    if (dir.is_absolute()) {
      return dir;
    }
  }
  return std::nullopt;
}
#endif

}  // namespace

Result<LoadedSettings> parse_settings(std::istream& in, const std::string& source,
                                      const fs::path& base_dir) {
  auto parsed = toml::try_parse(in, source);
  if (parsed.is_err()) {
    std::string detail;
    for (const auto& e : parsed.unwrap_err()) {
      detail += toml::format_error(e);
    }
    return fail(ErrCode::validation, std::move(detail));
  }
  const toml::value& root = parsed.unwrap();

  LoadedSettings out;
  auto& s = out.values;
  CFD_TRY(server, string_key(root, "server", source));
  if (server) {
    s.server = std::move(*server);
    s.from.server = source;
  }
  CFD_TRY(user, string_key(root, "user", source));
  if (user) {
    s.user = std::move(*user);
    s.from.user = source;
  }
  CFD_TRY(ca_file, string_key(root, "ca_file", source));
  if (ca_file) {
    // Relative to the file, so the result doesn't depend on the working
    // directory ftc happens to run in. Empty stays empty: system CAs.
    fs::path p{*ca_file};
    if (!p.empty() && p.is_relative() && !base_dir.empty()) {
      p = (base_dir / p).lexically_normal();
    }
    s.ca_file = std::move(p);
    s.from.ca_file = source;
  }

  for (const auto& [key, _] : root.as_table()) {
    if (key == "server" || key == "user" || key == "ca_file") {
      continue;
    }
    if (key == "allow_http" || key == "allow-http") {
      out.warnings.push_back(std::format(
          "{}: {}: ignored, plain http is allowed only with --allow-http", source, key));
    } else if (key == "password") {
      out.warnings.push_back(
          std::format("{}: {}: ignored, passwords are never read from config files", source, key));
    } else {
      out.warnings.push_back(std::format("{}: {}: unknown key, ignored", source, key));
    }
  }
  return out;
}

Result<LoadedSettings> load_settings_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return fail(ErrCode::validation, std::format("cannot read config file '{}'", path.string()));
  }
  std::error_code ec;
  const fs::path abs = fs::absolute(path, ec);
  return parse_settings(in, path.string(), ec ? path.parent_path() : abs.parent_path());
}

Result<std::optional<LoadedSettings>> load_settings_file_if_exists(const fs::path& path) {
  std::error_code ec;
  if (!fs::exists(path, ec) && !ec) {
    return std::nullopt;
  }
  CFD_TRY(loaded, load_settings_file(path));
  return std::optional<LoadedSettings>{std::move(loaded)};
}

Settings overlay(Settings low, const Settings& high) {
  if (high.server) {
    low.server = high.server;
    low.from.server = high.from.server;
  }
  if (high.user) {
    low.user = high.user;
    low.from.user = high.from.user;
  }
  if (high.ca_file) {
    low.ca_file = high.ca_file;
    low.from.ca_file = high.from.ca_file;
  }
  return low;
}

std::vector<fs::path> default_settings_files() {
  std::vector<fs::path> files;
#ifdef _WIN32
  if (auto dir = env_dir(L"PROGRAMDATA")) {
    files.push_back(*dir / "ftc" / "ftc.toml");
  }
  if (auto dir = env_dir(L"APPDATA")) {
    files.push_back(*dir / "ftc" / "ftc.toml");
  }
#else
  files.emplace_back(FTC_SYSTEM_CONFIG);
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME");  // NOLINT(concurrency-mt-unsafe)
      xdg != nullptr && *xdg == '/') {
    files.push_back(fs::path{xdg} / "ftc" / "ftc.toml");
  } else if (const char* home = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe)
             home != nullptr && *home != '\0') {
    files.push_back(fs::path{home} / ".config" / "ftc" / "ftc.toml");
  }
#endif
  return files;
}

}  // namespace confide::client
