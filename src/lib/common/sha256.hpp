#pragma once
#include "common/error.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

// OpenSSL context type, kept out of this header.
struct evp_md_ctx_st;

namespace confide::common {

using ByteSpan = std::span<const std::byte>;

// SHA-256 digest of a file (NFR-6). Travels as 64 hex characters in the
// X-Checksum-SHA256 header, manifest.json and peer metadata; compared as
// bytes, so upper- and lowercase hex name the same digest.
class Sha256Digest {
public:
  static constexpr std::size_t kSize = 32;
  using Bytes = std::array<std::byte, kSize>;

  explicit Sha256Digest(const Bytes& bytes) : bytes_(bytes) {}

  // Untrusted text (request header, manifest, peer). Exactly 64 hex digits,
  // either case; anything else is `validation`.
  static Result<Sha256Digest> from_hex(std::string_view hex);

  // Always lowercase, like sha256sum.
  std::string to_hex() const;

  const Bytes& bytes() const noexcept { return bytes_; }

  auto operator<=>(const Sha256Digest&) const = default;

private:
  Bytes bytes_;
};

class Sha256 {
public:
  // Fails only if OpenSSL can't allocate or initialise (`internal`).
  static Result<Sha256> create();

  Sha256(Sha256&&) noexcept = default;
  Sha256& operator=(Sha256&&) noexcept = default;
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;
  ~Sha256() = default;

  Result<void> update(ByteSpan data);
  Result<void> update(std::string_view data);

  // Digest of everything fed so far; then resets for a new stream.
  Result<Sha256Digest> finish();

  // One-shot helper for small in-memory data.
  static Result<Sha256Digest> of(ByteSpan data);
  static Result<Sha256Digest> of(std::string_view data);

private:
  struct CtxDeleter {
    void operator()(evp_md_ctx_st* ctx) const noexcept;
  };

  explicit Sha256(std::unique_ptr<evp_md_ctx_st, CtxDeleter> ctx) : ctx_(std::move(ctx)) {}

  std::unique_ptr<evp_md_ctx_st, CtxDeleter> ctx_;
};

}  // namespace confide::common

// Hash specialization for Sha256Digest to be used in unordered containers.
// Hashes all bytes rather than taking a prefix: digests may come from a
// client, which could pick values that collide on a prefix.
template <>
struct std::hash<confide::common::Sha256Digest> {
  std::size_t operator()(const confide::common::Sha256Digest& d) const noexcept {
    const auto& b = d.bytes();
    return std::hash<std::string_view>{}(
        std::string_view{reinterpret_cast<const char*>(b.data()), b.size()});  // NOLINT
  }
};
