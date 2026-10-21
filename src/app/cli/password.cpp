#include "password.hpp"

#include <termios.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <cstdlib>
#include <format>
#include <iostream>
#include <utility>

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

// Reads one line from the terminal without echo.
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

}  // namespace

// FTC_PASSWORD is for development only: other processes of the same user can
// read it from /proc/PID/environ, and child processes inherit it.
Result<std::string> read_password(std::string_view user) {
  if (const char* env = std::getenv("FTC_PASSWORD");
      env != nullptr) {  // NOLINT(concurrency-mt-unsafe)
    return std::string{env};
  }

  std::string password;
  if (isatty(STDIN_FILENO) == 0) {
    std::getline(std::cin, password);
  } else {
    CFD_TRY(line, read_hidden(std::format("password for {}: ", user)));
    password = std::move(line);
  }
  if (password.ends_with('\r')) {
    password.pop_back();
  }
  if (password.empty()) {
    return fail(ErrCode::validation, "no password (set FTC_PASSWORD or type it)");
  }
  return password;
}

bool can_ask_password() {
  return std::getenv("FTC_PASSWORD") != nullptr  // NOLINT(concurrency-mt-unsafe)
         || isatty(STDIN_FILENO) != 0;
}

}  // namespace confide::cli
