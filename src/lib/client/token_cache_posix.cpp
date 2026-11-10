// POSIX half of TokenCache (token_cache_win.cpp is the other): folders
// 0700, files 0600, owned by the effective user.
#include "token_cache.hpp"

#include "token_cache_detail.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <format>
#include <system_error>
#include <utility>

namespace confide::client {

namespace fs = std::filesystem;

namespace {

std::unexpected<Error> sys_fail(std::string_view what, const fs::path& p) {
  return fail(ErrCode::io,
              std::format("{} {}: {}", what, p.string(), std::generic_category().message(errno)));
}

// Closes the descriptor when it goes out of scope.
class Fd {
public:
  explicit Fd(int fd) : fd_(fd) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&&) = delete;
  Fd& operator=(Fd&&) = delete;
  ~Fd() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  int get() const { return fd_; }
  // close() reports write errors that fsync may not have.
  bool close() { return ::close(std::exchange(fd_, -1)) == 0; }

private:
  int fd_;
};

// Owned by us and not open to group or others.
Result<void> check_private(const struct stat& st, const fs::path& p, std::string_view fix) {
  if (st.st_uid != ::geteuid()) {
    return fail(ErrCode::validation, std::format("{} is not owned by you", p.string()));
  }
  if ((st.st_mode & 077) != 0) {
    return fail(ErrCode::validation, std::format("{} is accessible by other users; run: {} {}",
                                                 p.string(), fix, p.string()));
  }
  return {};
}

}  // namespace

Result<fs::path> detail::state_dir() {
  if (const char* xdg = std::getenv("XDG_STATE_HOME");  // NOLINT(concurrency-mt-unsafe)
      xdg != nullptr && *xdg == '/') {
    return fs::path{xdg};
  }
  if (const char* home = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe)
      home != nullptr && *home != '\0') {
    return fs::path{home} / ".local" / "state";
  }
  return fail(ErrCode::validation, "can't find a place for the token: set HOME");
}

Result<void> TokenCache::prepare_dir() const {
  const fs::path tokens = file_.parent_path();
  const fs::path ftc = tokens.parent_path();
  std::error_code ec;
  fs::create_directories(ftc.parent_path(), ec);
  if (ec) {
    return fail(ErrCode::io, std::format("{}: {}", ftc.parent_path().string(), ec.message()));
  }
  for (const auto& dir : {ftc, tokens}) {
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
      return sys_fail("create", dir);
    }
    struct stat st{};
    if (::lstat(dir.c_str(), &st) != 0) {
      return sys_fail("stat", dir);
    }
    if (!S_ISDIR(st.st_mode)) {
      return fail(ErrCode::validation, std::format("{} is not a folder", dir.string()));
    }
    CFD_TRYV(check_private(st, dir, "chmod 700"));
  }
  return {};
}

Result<std::optional<std::string>> TokenCache::load() const {
  Fd fd{::open(file_.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
  if (fd.get() < 0) {
    if (errno == ENOENT) {
      return std::nullopt;
    }
    if (errno == ELOOP) {
      return fail(ErrCode::validation, std::format("{} is a symlink", file_.string()));
    }
    return sys_fail("open", file_);
  }
  struct stat st{};
  if (::fstat(fd.get(), &st) != 0) {
    return sys_fail("stat", file_);
  }
  if (!S_ISREG(st.st_mode)) {
    return fail(ErrCode::validation, std::format("{} is not a file", file_.string()));
  }
  CFD_TRYV(check_private(st, file_, "chmod 600"));

  std::array<char, detail::kMaxTokenBytes> buf{};
  const auto n = ::read(fd.get(), buf.data(), buf.size());
  if (n < 0) {
    return sys_fail("read", file_);
  }
  return detail::token_from({buf.data(), static_cast<std::size_t>(n)});
}

Result<void> TokenCache::save(std::string_view token) const {
  CFD_TRYV(prepare_dir());
  fs::path tmp = file_;
  tmp += ".tmp";
  ::unlink(tmp.c_str());  // left over from a crash; O_EXCL below needs it gone

  auto written = [&]() -> Result<void> {
    Fd fd{::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)};
    if (fd.get() < 0) {
      return sys_fail("create", tmp);
    }
    if (::fchmod(fd.get(), 0600) != 0) {  // in case the umask was odd
      return sys_fail("chmod", tmp);
    }
    const std::string line = std::string{token} + '\n';
    if (::write(fd.get(), line.data(), line.size()) != static_cast<ssize_t>(line.size())
        || ::fsync(fd.get()) != 0 || !fd.close()) {
      return sys_fail("write", tmp);
    }
    if (::rename(tmp.c_str(), file_.c_str()) != 0) {
      return sys_fail("rename", file_);
    }
    return {};
  };
  auto res = written();
  if (!res) {
    ::unlink(tmp.c_str());
  }
  return res;
}

Result<void> TokenCache::remove() const {
  if (::unlink(file_.c_str()) != 0 && errno != ENOENT) {
    return sys_fail("remove", file_);
  }
  return {};
}

}  // namespace confide::client
