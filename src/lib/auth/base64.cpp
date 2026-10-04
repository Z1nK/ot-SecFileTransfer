#include "auth/base64.hpp"

#include <algorithm>
#include <cstdint>

namespace confide::auth {

namespace {

using common::ErrCode;
using common::Error;

constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 6-bit value of a base64 digit, -1 if `c` is not one.
int digit(char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (c == '+') {
    return 62;
  }
  if (c == '/') {
    return 63;
  }
  return -1;
}

std::unexpected<Error> invalid(std::string detail) {
  return std::unexpected(Error::make(ErrCode::validation, std::move(detail)));
}

}  // namespace

std::string base64_encode(common::ByteSpan data) {
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  for (std::size_t i = 0; i < data.size(); i += 3) {
    const std::size_t n = std::min<std::size_t>(3, data.size() - i);
    std::uint32_t group = 0;
    for (std::size_t k = 0; k < 3; ++k) {
      group = (group << 8) | (k < n ? std::to_integer<std::uint32_t>(data[i + k]) : 0U);
    }
    // n input bytes give n + 1 digits; the rest of the 4 is padding.
    for (std::size_t k = 0; k < 4; ++k) {
      out += k <= n ? kAlphabet[(group >> (18 - 6 * k)) & 0x3FU] : '=';
    }
  }
  return out;
}

Result<std::vector<std::byte>> base64_decode(std::string_view text) {
  if (text.size() % 4 != 0) {
    return invalid("base64: length is not a multiple of 4");
  }
  std::vector<std::byte> out;
  out.reserve(text.size() / 4 * 3);
  for (std::size_t i = 0; i < text.size(); i += 4) {
    const bool last = i + 4 == text.size();
    std::size_t pad = 0;
    std::uint32_t group = 0;
    for (std::size_t k = 0; k < 4; ++k) {
      const char c = text[i + k];
      if (c == '=') {
        // At most two, only in the last group: "xx==" or "xxx=".
        if (!last || k < 2) {
          return invalid("base64: misplaced padding");
        }
        ++pad;
        group <<= 6;
        continue;
      }
      const int d = digit(c);
      if (d < 0 || pad > 0) {
        return invalid("base64: invalid character");
      }
      group = (group << 6) | static_cast<std::uint32_t>(d);
    }
    for (std::size_t k = 0; k < 3 - pad; ++k) {
      out.push_back(static_cast<std::byte>((group >> (16 - 8 * k)) & 0xFFU));
    }
  }
  return out;
}

}  // namespace confide::auth
