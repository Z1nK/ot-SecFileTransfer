// POSIX half of platform.hpp (platform_win.cpp is the other).
#include "platform.hpp"

#include <pwd.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <iostream>
#include <vector>

namespace confide::cli {

namespace {

// Terminal settings to put back if a signal ends ftc while echo is off;
// otherwise the shell is left without echo.
termios saved_tty{};

void restore_tty_and_reraise(int signo) {
  tcsetattr(STDIN_FILENO, TCSANOW, &saved_tty);
  [[maybe_unused]] const auto n = write(STDERR_FILENO, "\n", 1);
  std::signal(signo, SIG_DFL);  // NOLINT(cert-msc54-cpp)
  std::raise(signo);            // delivered with SIG_DFL once the handler returns
}

}  // namespace

bool stdin_is_terminal() { return isatty(STDIN_FILENO) != 0; }

Result<std::string> read_hidden(std::string_view prompt) {
  if (tcgetattr(STDIN_FILENO, &saved_tty) != 0) {
    return fail(ErrCode::io, "cannot read the terminal settings");
  }
  constexpr std::array kSignals{SIGINT, SIGTERM, SIGQUIT, SIGHUP};
  std::array<struct sigaction, kSignals.size()> old_actions{};
  struct sigaction restore {};
  restore.sa_handler = restore_tty_and_reraise;
  sigemptyset(&restore.sa_mask);
  for (std::size_t i = 0; i < kSignals.size(); ++i) {
    sigaction(kSignals[i], &restore, &old_actions[i]);
  }

  std::cerr << prompt << std::flush;
  termios quiet = saved_tty;
  quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
  std::string line;
  std::getline(std::cin, line);
  tcsetattr(STDIN_FILENO, TCSANOW, &saved_tty);
  std::cerr << '\n';

  for (std::size_t i = 0; i < kSignals.size(); ++i) {
    sigaction(kSignals[i], &old_actions[i], nullptr);
  }
  return line;
}

std::string system_user() {
  const long hint = sysconf(_SC_GETPW_R_SIZE_MAX);
  std::vector<char> buf(hint > 0 ? static_cast<std::size_t>(hint) : 16384);
  passwd pwd{};
  passwd* found = nullptr;
  if (getpwuid_r(geteuid(), &pwd, buf.data(), buf.size(), &found) != 0 || found == nullptr) {
    return {};
  }
  return found->pw_name;
}

}  // namespace confide::cli
