#pragma once
// The few terminal and account calls of ftc that differ between POSIX and
// Windows: platform_posix.cpp or platform_win.cpp (CMake builds one).
#include "cli.hpp"

#include <string>
#include <string_view>

namespace confide::cli {

// stdin is an interactive terminal (a console on Windows).
bool stdin_is_terminal();

// Shows the prompt on stderr and reads one line from the terminal without
// echo. Echo comes back on afterwards, also when Ctrl+C or a signal ends
// ftc while it waits. The text is UTF-8, without the line end.
Result<std::string> read_hidden(std::string_view prompt);

// The account ftc runs as, in UTF-8: from the passwd database on POSIX
// (NSS, so LDAP accounts too, not $USER), GetUserNameW() on Windows. Empty
// if the lookup fails.
std::string system_user();

}  // namespace confide::cli
