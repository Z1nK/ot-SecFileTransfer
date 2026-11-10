// Windows half of platform.hpp (platform_posix.cpp is the other).
#include "platform.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <lmcons.h>  // UNLEN

#include <array>
#include <iostream>

namespace confide::cli {

namespace {

// Console mode to put back if Ctrl+C or closing the window ends ftc while
// echo is off; the console keeps its mode after ftc exits.
HANDLE console_in = nullptr;
DWORD saved_mode = 0;

BOOL WINAPI restore_console(DWORD /*event*/) {
  ::SetConsoleMode(console_in, saved_mode);
  return FALSE;  // go on to the default handler, which ends ftc
}

std::string to_utf8(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const int size = static_cast<int>(text.size());
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), size, nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), size, out.data(), n, nullptr, nullptr);
  return out;
}

}  // namespace

bool stdin_is_terminal() {
  DWORD mode = 0;
  return ::GetConsoleMode(::GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
}

Result<std::string> read_hidden(std::string_view prompt) {
  console_in = ::GetStdHandle(STD_INPUT_HANDLE);
  if (::GetConsoleMode(console_in, &saved_mode) == 0) {
    return fail(ErrCode::io, "cannot read the console settings");
  }
  ::SetConsoleCtrlHandler(restore_console, TRUE);

  std::cerr << prompt << std::flush;
  ::SetConsoleMode(console_in, (saved_mode | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT)
                                   & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));
  // ReadConsoleW rather than std::cin: it hands over UTF-16 whatever the
  // console code page, so a non-ASCII password reaches the server as UTF-8,
  // as on Linux. Stops at the line end, or empty-handed on Ctrl+C.
  std::wstring line;
  std::array<wchar_t, 256> buf{};
  while (line.find(L'\n') == std::wstring::npos) {
    DWORD n = 0;
    if (::ReadConsoleW(console_in, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) == 0
        || n == 0) {
      break;
    }
    line.append(buf.data(), n);
  }
  ::SecureZeroMemory(buf.data(), sizeof(buf));
  ::SetConsoleMode(console_in, saved_mode);
  ::SetConsoleCtrlHandler(restore_console, FALSE);
  std::cerr << '\n';

  if (const auto end = line.find_first_of(L"\r\n"); end != std::wstring::npos) {
    line.resize(end);
  }
  std::string out = to_utf8(line);
  ::SecureZeroMemory(line.data(), line.size() * sizeof(wchar_t));
  return out;
}

std::string system_user() {
  std::array<wchar_t, UNLEN + 1> name{};
  auto size = static_cast<DWORD>(name.size());
  if (::GetUserNameW(name.data(), &size) == 0 || size <= 1) {
    return {};
  }
  return to_utf8({name.data(), size - 1});  // size counts the closing NUL
}

}  // namespace confide::cli
