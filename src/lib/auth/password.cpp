#include "auth/password.hpp"

#include "auth/base64.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <charconv>
#include <format>

namespace confide::auth {

namespace {

using common::ErrCode;
using common::Error;

std::unexpected<Error> invalid(std::string detail) {
  return std::unexpected(Error::make(ErrCode::validation, std::move(detail)));
}

Result<std::vector<std::byte>> pbkdf2(std::string_view password, const std::vector<std::byte>& salt,
                                      std::uint32_t iterations, std::size_t key_bytes) {
  std::vector<std::byte> key(key_bytes);
  const int ok =
      PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                        reinterpret_cast<const unsigned char*>(salt.data()),
                        static_cast<int>(salt.size()), static_cast<int>(iterations), EVP_sha256(),
                        static_cast<int>(key.size()), reinterpret_cast<unsigned char*>(key.data()));
  if (ok != 1) {
    return std::unexpected(Error::make(ErrCode::internal, "PKCS5_PBKDF2_HMAC failed"));
  }
  return key;
}

Result<void> check_iterations(std::uint32_t iterations) {
  if (iterations < 1 || iterations > PasswordHash::kMaxIterations) {
    return invalid(std::format("iterations must be 1..{}", PasswordHash::kMaxIterations));
  }
  return {};
}

}  // namespace

Result<PasswordHash> PasswordHash::create(std::string_view password, std::uint32_t iterations) {
  CFD_TRYV(check_iterations(iterations));
  std::vector<std::byte> salt(kSaltBytes);
  if (RAND_bytes(reinterpret_cast<unsigned char*>(salt.data()), static_cast<int>(salt.size()))
      != 1) {
    return std::unexpected(Error::make(ErrCode::internal, "RAND_bytes failed"));
  }
  CFD_TRY(key, pbkdf2(password, salt, iterations, kKeyBytes));
  return PasswordHash(iterations, std::move(salt), std::move(key));
}

Result<PasswordHash> PasswordHash::parse(std::string_view text) {
  // scheme $ iterations $ salt $ key
  std::vector<std::string_view> parts;
  for (std::size_t start = 0;;) {
    const auto end = text.find('$', start);
    parts.push_back(text.substr(start, end - start));
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  if (parts.size() != 4 || parts[0] != kScheme) {
    return invalid(std::format("expected {}$<iterations>$<salt>$<key>", kScheme));
  }

  std::uint32_t iterations = 0;
  const auto [ptr, ec] =
      std::from_chars(parts[1].data(), parts[1].data() + parts[1].size(), iterations);
  if (ec != std::errc{} || ptr != parts[1].data() + parts[1].size()) {
    return invalid("iterations is not a number");
  }
  CFD_TRYV(check_iterations(iterations));

  CFD_TRY(salt, base64_decode(parts[2]));
  CFD_TRY(key, base64_decode(parts[3]));
  if (salt.empty() || key.empty()) {
    return invalid("empty salt or key");
  }
  return PasswordHash(iterations, std::move(salt), std::move(key));
}

std::string PasswordHash::to_string() const {
  return std::format("{}${}${}${}", kScheme, iterations_, base64_encode(salt_),
                     base64_encode(key_));
}

bool PasswordHash::verify(std::string_view password) const {
  auto derived = pbkdf2(password, salt_, iterations_, key_.size());
  return derived && CRYPTO_memcmp(derived->data(), key_.data(), key_.size()) == 0;
}

}  // namespace confide::auth
