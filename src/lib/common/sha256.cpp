#include "common/sha256.hpp"

#include <openssl/evp.h>

#include <cstdint>
#include <utility>

namespace confide::common {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

int hex_nibble(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

std::unexpected<Error> openssl_failed(std::string_view what) {
  return std::unexpected(Error::make(ErrCode::internal, std::string{"sha256: "} += what));
}

ByteSpan as_bytes(std::string_view s) {
  return std::as_bytes(std::span{s.data(), s.size()});
}

}  // namespace

Result<Sha256Digest> Sha256Digest::from_hex(std::string_view hex) {
  if (hex.size() != kSize * 2) {
    return std::unexpected(Error::make(ErrCode::validation, "sha256 must be 64 hex digits"));
  }
  Bytes out{};
  for (std::size_t i = 0; i < kSize; ++i) {
    const int hi = hex_nibble(hex[2 * i]);
    const int lo = hex_nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      return std::unexpected(Error::make(ErrCode::validation, "sha256 has non-hex character"));
    }
    out[i] = static_cast<std::byte>((hi << 4) | lo);
  }
  return Sha256Digest{out};
}

std::string Sha256Digest::to_hex() const {
  std::string out(kSize * 2, '\0');
  for (std::size_t i = 0; i < kSize; ++i) {
    const auto byte = static_cast<std::uint8_t>(bytes_[i]);
    out[2 * i] = kHexDigits[byte >> 4];
    out[2 * i + 1] = kHexDigits[byte & 0x0F];
  }
  return out;
}

void Sha256::CtxDeleter::operator()(evp_md_ctx_st* ctx) const noexcept {
  EVP_MD_CTX_free(ctx);
}

Result<Sha256> Sha256::create() {
  std::unique_ptr<evp_md_ctx_st, CtxDeleter> ctx{EVP_MD_CTX_new()};
  if (!ctx) {
    return openssl_failed("EVP_MD_CTX_new");
  }
  if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
    return openssl_failed("EVP_DigestInit_ex");
  }
  return Sha256{std::move(ctx)};
}

Result<void> Sha256::update(ByteSpan data) {
  if (!ctx_) {
    return openssl_failed("use after move");
  }
  if (EVP_DigestUpdate(ctx_.get(), data.data(), data.size()) != 1) {
    return openssl_failed("EVP_DigestUpdate");
  }
  return {};
}

Result<void> Sha256::update(std::string_view data) {
  return update(as_bytes(data));
}

Result<Sha256Digest> Sha256::finish() {
  if (!ctx_) {
    return openssl_failed("use after move");
  }
  Sha256Digest::Bytes out{};
  unsigned int len = 0;
  if (EVP_DigestFinal_ex(ctx_.get(), reinterpret_cast<unsigned char*>(out.data()), &len) != 1 ||  // NOLINT
      len != Sha256Digest::kSize) {
    return openssl_failed("EVP_DigestFinal_ex");
  }
  if (EVP_DigestInit_ex(ctx_.get(), EVP_sha256(), nullptr) != 1) {
    return openssl_failed("EVP_DigestInit_ex");
  }
  return Sha256Digest{out};
}

Result<Sha256Digest> Sha256::of(ByteSpan data) {
  CFD_TRY(h, create());
  CFD_TRYV(h.update(data));
  return h.finish();
}

Result<Sha256Digest> Sha256::of(std::string_view data) {
  return of(as_bytes(data));
}

}  // namespace confide::common
