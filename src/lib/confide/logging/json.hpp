#pragma once

#include <string>
#include <string_view>

namespace confide::logging {

// Escapes `s` for use inside a JSON string literal (without the quotes).
// Shared by TechLog and BusinessLog so both produce valid JSON lines.
std::string json_escape(std::string_view s);

}  // namespace confide::logging
