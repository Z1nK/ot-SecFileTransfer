// Windows half of TokenCache (token_cache_posix.cpp is the other): the
// tokens live under %LOCALAPPDATA%\ftc\tokens. The folders and the file are
// created owned by the current user, with a protected DACL (no inherited
// entries) that grants only that user. As with 0700/0600 on POSIX, a folder
// or file owned by someone else, or open to other accounts, is refused.
// SYSTEM and Administrators are let through: they can take any file anyway.
#include "token_cache.hpp"

#include "token_cache_detail.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

#include <array>
#include <cstdlib>
#include <format>
#include <memory>
#include <system_error>
#include <vector>

namespace confide::client {

namespace fs = std::filesystem;

namespace {

// UTF-8 for messages; path::string() would convert to the ANSI code page
// and throws on characters it can't map.
std::string shown(const fs::path& p) {
  auto u8 = p.u8string();
  return {u8.begin(), u8.end()};
}

std::string win_message(DWORD err) {
  return std::system_category().message(static_cast<int>(err));
}

std::unexpected<Error> win_fail(std::string_view what, const fs::path& p,
                                DWORD err = ::GetLastError()) {
  return fail(ErrCode::io, std::format("{} {}: {}", what, shown(p), win_message(err)));
}

struct HandleCloser {
  void operator()(HANDLE h) const noexcept { ::CloseHandle(h); }
};
using Handle = std::unique_ptr<void, HandleCloser>;

// CreateFileW reports failure as INVALID_HANDLE_VALUE, not null.
Handle own(HANDLE h) { return Handle{h == INVALID_HANDLE_VALUE ? nullptr : h}; }

struct LocalFreer {
  void operator()(void* p) const noexcept { ::LocalFree(p); }
};
using SecurityDescriptor = std::unique_ptr<void, LocalFreer>;

// The SID of the account ftc runs as.
class UserSid {
public:
  static Result<UserSid> current() {
    HANDLE raw = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw) == 0) {
      return fail(ErrCode::io,
                  std::format("can't read the process token: {}", win_message(::GetLastError())));
    }
    const Handle token{raw};
    DWORD size = 0;
    ::GetTokenInformation(raw, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buf(size);
    if (size == 0 || ::GetTokenInformation(raw, TokenUser, buf.data(), size, &size) == 0) {
      return fail(ErrCode::io,
                  std::format("can't read the current user: {}", win_message(::GetLastError())));
    }
    return UserSid{std::move(buf)};
  }

  PSID get() const {
    // TOKEN_USER and the SID it points to share the buffer.
    return reinterpret_cast<const TOKEN_USER*>(buf_.data())->User.Sid;  // NOLINT
  }

private:
  explicit UserSid(std::vector<std::byte> buf) : buf_(std::move(buf)) {}
  std::vector<std::byte> buf_;
};

// Owner: the user. DACL: protected, full access for the user only; OICI so
// the files created in a folder get the same entry.
Result<SecurityDescriptor> private_descriptor(const UserSid& user) {
  wchar_t* sid_text = nullptr;
  if (::ConvertSidToStringSidW(user.get(), &sid_text) == 0) {
    return fail(ErrCode::io, std::format("can't convert the user SID: {}",
                                         win_message(::GetLastError())));
  }
  const std::wstring sid{sid_text};
  ::LocalFree(sid_text);
  const std::wstring sddl = L"O:" + sid + L"D:P(A;OICI;FA;;;" + sid + L")";
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd,
                                                             nullptr)
      == 0) {
    return fail(ErrCode::io, std::format("can't build the security descriptor: {}",
                                         win_message(::GetLastError())));
  }
  return SecurityDescriptor{sd};
}

SECURITY_ATTRIBUTES attributes(const SecurityDescriptor& sd) {
  return SECURITY_ATTRIBUTES{.nLength = static_cast<DWORD>(sizeof(SECURITY_ATTRIBUTES)),
                             .lpSecurityDescriptor = sd.get(),
                             .bInheritHandle = FALSE};
}

// Owned by the user, and the DACL lets in no one else but SYSTEM and
// Administrators.
Result<void> check_private(PSID owner, PACL dacl, const UserSid& user, const fs::path& p,
                           bool folder) {
  if (owner == nullptr || ::EqualSid(owner, user.get()) == 0) {
    return fail(ErrCode::validation, std::format("{} is not owned by you", shown(p)));
  }
  bool open = dacl == nullptr;  // a NULL DACL lets everyone in
  for (DWORD i = 0; !open && i < dacl->AceCount; ++i) {
    void* ace = nullptr;
    if (::GetAce(dacl, i, &ace) == 0) {
      open = true;
      break;
    }
    const auto type = static_cast<const ACE_HEADER*>(ace)->AceType;
    if (type == ACCESS_DENIED_ACE_TYPE) {
      continue;
    }
    if (type != ACCESS_ALLOWED_ACE_TYPE) {  // object or callback entries: don't guess
      open = true;
      break;
    }
    PSID sid = &static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart;
    open = ::EqualSid(sid, user.get()) == 0 && ::IsWellKnownSid(sid, WinLocalSystemSid) == 0
           && ::IsWellKnownSid(sid, WinBuiltinAdministratorsSid) == 0;
  }
  if (open) {
    return fail(ErrCode::validation,
                std::format(R"({} is accessible by other users; run: icacls "{}" /inheritance:r /grant:r "%USERNAME%:{}F")",
                            shown(p), shown(p), folder ? "(OI)(CI)" : ""));
  }
  return {};
}

Result<void> check_private(const fs::path& p, const UserSid& user, bool folder) {
  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR raw = nullptr;
  const DWORD rc = ::GetNamedSecurityInfoW(p.c_str(), SE_FILE_OBJECT,
                                           OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                           &owner, nullptr, &dacl, nullptr, &raw);
  if (rc != ERROR_SUCCESS) {
    return win_fail("read the permissions of", p, rc);
  }
  const SecurityDescriptor sd{raw};
  return check_private(owner, dacl, user, p, folder);
}

Result<void> check_private(HANDLE h, const fs::path& p, const UserSid& user) {
  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR raw = nullptr;
  const DWORD rc = ::GetSecurityInfo(h, SE_FILE_OBJECT,
                                     OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                     &owner, nullptr, &dacl, nullptr, &raw);
  if (rc != ERROR_SUCCESS) {
    return win_fail("read the permissions of", p, rc);
  }
  const SecurityDescriptor sd{raw};
  return check_private(owner, dacl, user, p, false);
}

struct Freer {
  void operator()(void* p) const noexcept { std::free(p); }  // NOLINT(cppcoreguidelines-no-malloc)
};

}  // namespace

Result<fs::path> detail::state_dir() {
  wchar_t* value = nullptr;
  std::size_t len = 0;
  if (::_wdupenv_s(&value, &len, L"LOCALAPPDATA") == 0 && value != nullptr) {
    const std::unique_ptr<wchar_t, Freer> owned{value};
    fs::path dir{value};
    if (dir.is_absolute()) {
      return dir;
    }
  }
  return fail(ErrCode::validation, "can't find a place for the token: set LOCALAPPDATA");
}

Result<void> TokenCache::prepare_dir() const {
  const fs::path tokens = file_.parent_path();
  const fs::path ftc = tokens.parent_path();
  std::error_code ec;
  fs::create_directories(ftc.parent_path(), ec);
  if (ec) {
    return fail(ErrCode::io, std::format("{}: {}", shown(ftc.parent_path()), ec.message()));
  }
  CFD_TRY(user, UserSid::current());
  CFD_TRY(sd, private_descriptor(user));
  SECURITY_ATTRIBUTES sa = attributes(sd);
  for (const auto& dir : {ftc, tokens}) {
    if (::CreateDirectoryW(dir.c_str(), &sa) == 0 && ::GetLastError() != ERROR_ALREADY_EXISTS) {
      return win_fail("create", dir);
    }
    const DWORD attrs = ::GetFileAttributesW(dir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
      return win_fail("stat", dir);
    }
    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      return fail(ErrCode::validation, std::format("{} is not a folder", shown(dir)));
    }
    CFD_TRYV(check_private(dir, user, true));
  }
  return {};
}

Result<std::optional<std::string>> TokenCache::load() const {
  // FILE_SHARE_DELETE: a save() running meanwhile can still replace the file.
  const Handle file =
      own(::CreateFileW(file_.c_str(), GENERIC_READ | READ_CONTROL,
                        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!file) {
    const DWORD err = ::GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
      return std::nullopt;
    }
    return win_fail("open", file_, err);
  }
  BY_HANDLE_FILE_INFORMATION info{};
  if (::GetFileInformationByHandle(file.get(), &info) == 0) {
    return win_fail("stat", file_);
  }
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return fail(ErrCode::validation, std::format("{} is a symlink", shown(file_)));
  }
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return fail(ErrCode::validation, std::format("{} is not a file", shown(file_)));
  }
  CFD_TRY(user, UserSid::current());
  CFD_TRYV(check_private(file.get(), file_, user));

  std::array<char, detail::kMaxTokenBytes> buf{};
  DWORD n = 0;
  if (::ReadFile(file.get(), buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) == 0) {
    return win_fail("read", file_);
  }
  return detail::token_from({buf.data(), n});
}

Result<void> TokenCache::save(std::string_view token) const {
  CFD_TRYV(prepare_dir());
  CFD_TRY(user, UserSid::current());
  CFD_TRY(sd, private_descriptor(user));
  SECURITY_ATTRIBUTES sa = attributes(sd);
  fs::path tmp = file_;
  tmp += ".tmp";
  ::DeleteFileW(tmp.c_str());  // left over from a crash; CREATE_NEW below needs it gone

  auto written = [&]() -> Result<void> {
    Handle file = own(
        ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
      return win_fail("create", tmp);
    }
    const std::string line = std::string{token} + '\n';
    DWORD n = 0;
    if (::WriteFile(file.get(), line.data(), static_cast<DWORD>(line.size()), &n, nullptr) == 0
        || n != line.size() || ::FlushFileBuffers(file.get()) == 0
        || ::CloseHandle(file.release()) == 0) {
      return win_fail("write", tmp);
    }
    if (::MoveFileExW(tmp.c_str(), file_.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
        == 0) {
      return win_fail("rename", file_);
    }
    return {};
  };
  auto res = written();
  if (!res) {
    ::DeleteFileW(tmp.c_str());
  }
  return res;
}

Result<void> TokenCache::remove() const {
  if (::DeleteFileW(file_.c_str()) == 0) {
    const DWORD err = ::GetLastError();
    if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
      return win_fail("remove", file_, err);
    }
  }
  return {};
}

}  // namespace confide::client
