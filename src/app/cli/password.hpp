#pragma once
// Getting the password without it showing up in `ps` or on the screen.
#include "cli.hpp"

#include <string>
#include <string_view>

namespace confide::cli {

// From FTC_PASSWORD (development only), else asked for on the terminal
// without echo, else the first line of stdin. `validation` if empty.
Result<std::string> read_password(std::string_view user);

// A password can be had without reading stdin, which may carry other data
// by then: re-login after the token expired is only tried then.
bool can_ask_password();

}  // namespace confide::cli
