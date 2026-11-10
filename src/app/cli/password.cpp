#include "password.hpp"

#include "platform.hpp"

#include <cstdlib>
#include <format>
#include <iostream>
#include <utility>

namespace confide::cli {

// FTC_PASSWORD is for development only: other processes of the same user can
// read it from /proc/PID/environ, and child processes inherit it.
Result<std::string> read_password(std::string_view user) {
  if (const char* env = std::getenv("FTC_PASSWORD");
      env != nullptr) {  // NOLINT(concurrency-mt-unsafe)
    return std::string{env};
  }

  std::string password;
  if (!stdin_is_terminal()) {
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
         || stdin_is_terminal();
}

}  // namespace confide::cli
