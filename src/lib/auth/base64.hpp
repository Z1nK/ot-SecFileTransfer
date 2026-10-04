#pragma once
#include "common/error.hpp"
#include "common/sha256.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace confide::auth {

using common::Result;

// RFC 4648 base64, standard alphabet, '=' padded. Used for HTTP Basic
// credentials and the salt / key of a PasswordHash.
std::string base64_encode(common::ByteSpan data);

// Strict: length a multiple of 4, padding only at the end, no whitespace.
// `validation` otherwise.
Result<std::vector<std::byte>> base64_decode(std::string_view text);

}  // namespace confide::auth
