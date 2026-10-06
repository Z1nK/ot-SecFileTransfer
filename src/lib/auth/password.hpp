#pragma once
#include "common/error.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace confide::auth {

using common::Result;

// A stored password (config `password_hash`, FR-5), PBKDF2-HMAC-SHA256:
//
//   pbkdf2-sha256$<iterations>$<salt, base64>$<key, base64>
//
// Generate one with `confide-passwd`. Plaintext passwords never reach the
// config; config rejects a `password` key.
class PasswordHash {
public:
  static constexpr std::string_view kScheme = "pbkdf2-sha256";
  static constexpr std::uint32_t kDefaultIterations = 600'000;  // OWASP, 2023
  static constexpr std::uint32_t kMaxIterations = 10'000'000;
  static constexpr std::size_t kSaltBytes = 16;
  static constexpr std::size_t kKeyBytes = 32;

  // Hashes `password` with a fresh random salt. `validation` unless
  // 1 <= iterations <= kMaxIterations, `internal` if OpenSSL fails.
  static Result<PasswordHash> create(std::string_view password,
                                     std::uint32_t iterations = kDefaultIterations);

  // Parses the stored form. `validation` on anything malformed.
  static Result<PasswordHash> parse(std::string_view text);

  // The stored form, as parse() reads it.
  std::string to_string() const;

  // Re-derives the key from `password` (slow by design) and compares it in
  // constant time.
  bool verify(std::string_view password) const;

  std::uint32_t iterations() const noexcept { return iterations_; }
  const std::vector<std::byte>& salt() const noexcept { return salt_; }

private:
  PasswordHash(std::uint32_t iterations, std::vector<std::byte> salt, std::vector<std::byte> key)
      : iterations_(iterations), salt_(std::move(salt)), key_(std::move(key)) {}

  std::uint32_t iterations_;
  std::vector<std::byte> salt_;
  std::vector<std::byte> key_;
};

}  // namespace confide::auth
