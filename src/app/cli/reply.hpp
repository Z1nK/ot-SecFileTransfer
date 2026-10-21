#pragma once
// Reading the server's JSON replies. Missing or mistyped fields read as
// empty/zero, so a newer server with extra or changed fields still lists.
#include "cli.hpp"

#include <boost/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace confide::cli {

namespace json = boost::json;

Result<json::object> parse_object(std::string_view text);

// The array under `key` in a list reply, e.g. {"files":[...]}.
Result<json::array> parse_list(std::string_view text, std::string_view key);

// "" if absent or not a string.
std::string_view str(const json::object& obj, std::string_view key);

// 0 if absent, negative or not a number.
std::uint64_t num(const json::object& obj, std::string_view key);

// "user@server", or just "user" if `server_key` is empty or absent.
std::string address(const json::object& tx, std::string_view user_key,
                    std::string_view server_key);

}  // namespace confide::cli
