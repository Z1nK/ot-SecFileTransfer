#include "recent_recipients.hpp"

#include "token_cache_detail.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <system_error>

// Not a secret like the tokens, so plain std::filesystem and fstreams on
// every OS. The recent/ folder is made inside the private ftc folder of
// TokenCache and only readable by the user on POSIX as well.

namespace confide::client {

namespace fs = std::filesystem;

namespace {

// UTF-8 for messages, as path::string() can throw on Windows.
std::string shown(const fs::path& p) {
  auto u8 = p.u8string();
  return {u8.begin(), u8.end()};
}

bool usable(std::string_view recipient) {
  return !recipient.empty() && recipient.find_first_of("\r\n") == std::string_view::npos;
}

}  // namespace

std::vector<std::string> detail::with_recent(std::vector<std::string> list,
                                             std::string_view recipient, std::size_t max) {
  if (usable(recipient)) {
    std::erase(list, recipient);
    list.insert(list.begin(), std::string{recipient});
  }
  if (list.size() > max) {
    list.resize(max);
  }
  return list;
}

Result<RecentRecipients> RecentRecipients::for_server(const ServerUrl& url, std::string_view user) {
  CFD_TRY(state, detail::state_dir());
  return RecentRecipients(state / "ftc" / "recent" / detail::server_file_name(url, user));
}

Result<std::vector<std::string>> RecentRecipients::load() const {
  std::ifstream in(file_, std::ios::binary);
  if (!in) {
    std::error_code ec;
    if (!fs::exists(file_, ec)) {
      return std::vector<std::string>{};
    }
    return fail(ErrCode::io, std::format("can't read {}", shown(file_)));
  }
  std::vector<std::string> list;
  std::string line;
  while (list.size() < kMax && std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (usable(line) && std::ranges::find(list, line) == list.end()) {
      list.push_back(line);
    }
  }
  return list;
}

Result<void> RecentRecipients::push(std::string_view recipient) const {
  const fs::path dir = file_.parent_path();
  std::error_code ec;
  // create_directory, not create_directories: the ftc folder must already
  // be there, made private by TokenCache.
  if (fs::create_directory(dir, ec)) {
    fs::permissions(dir, fs::perms::owner_all, ec);
  }
  if (ec) {
    return fail(ErrCode::io, std::format("create {}: {}", shown(dir), ec.message()));
  }

  CFD_TRY(old, load());
  const auto list = detail::with_recent(std::move(old), recipient);

  fs::path tmp = file_;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    for (const auto& r : list) {
      out << r << '\n';
    }
    out.close();
    if (!out) {
      fs::remove(tmp, ec);
      return fail(ErrCode::io, std::format("can't write {}", shown(tmp)));
    }
  }
  fs::rename(tmp, file_, ec);
  if (ec) {
    std::error_code ignored;
    fs::remove(tmp, ignored);
    return fail(ErrCode::io, std::format("rename {}: {}", shown(file_), ec.message()));
  }
  return {};
}

}  // namespace confide::client
