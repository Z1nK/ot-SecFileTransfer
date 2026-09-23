#pragma once

#include <cstdint>
#include <expected>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace confide::common {

// Failure categories shared by server and client. Each code means a distinct
// action for the caller (fix the request, log in, retry later, ...); put the
// specifics in Error::detail, not in new codes. The HTTP status in each
// comment is what `api` maps the code to.
enum class ErrCode : uint8_t {
  // 400. Malformed input: bad JSON body, bad `retention_days`, a file path
  // with `../` or an absolute path (common path sanitizer, FR-6, NFR-7),
  // invalid config file at startup (config).
  validation,

  // 401. Caller is not authenticated: wrong password, missing or bad token,
  // bad peer token on /internal/* (auth, FR-5).
  auth,

  // 403. Caller is authenticated but is neither sender nor target of the
  // transaction (transaction access rules, NFR-7). `api` may answer 404
  // instead so outsiders can't probe which ids exist.
  forbidden,

  // 404. No transaction with this id, or no such file in it (transaction,
  // storage).
  notfound,

  // 409. Operation not allowed in the transaction's current state: add or
  // delete a file after commit, commit twice (transaction state machine,
  // FR-3).
  conflict,

  // 410. Transaction exists but is `expired` and waiting for the retention
  // job to delete it (retention, FR-9). Once deleted it becomes `notfound`.
  gone,

  // 422. Content does not match its SHA-256: upload vs X-Checksum-SHA256,
  // re-check at commit (storage, transaction, NFR-6), or a download vs the
  // manifest in `ftc get` (client). The caller should upload again.
  integrity,

  // 422. `target_server` is not in PeerRegistry or `target_user` does not
  // exist there (FR-13, "destination unknown"). Raised on create and by the
  // forwarder. Retrying never helps, unlike `unavailable`.
  unknown_destination,

  // 503. A known peer or server can't be reached right now: the forwarder
  // retries with backoff and the transaction stays `committed` (FR-10);
  // `ftc` reports the server as down (net).
  unavailable,

  // 500. Disk read/write, directory create or atomic rename failed
  // (storage, transaction repository, logging file sink).
  io,

  // 500. Broken invariant / "can't happen" case. Always a bug.
  internal,
};

// Stable lowercase name of a code, for logs and diagnostics.
inline std::string_view to_string(ErrCode code) {
  switch (code) {
  case ErrCode::validation:
    return "validation";
  case ErrCode::auth:
    return "auth";
  case ErrCode::forbidden:
    return "forbidden";
  case ErrCode::notfound:
    return "notfound";
  case ErrCode::conflict:
    return "conflict";
  case ErrCode::gone:
    return "gone";
  case ErrCode::integrity:
    return "integrity";
  case ErrCode::unknown_destination:
    return "unknown_destination";
  case ErrCode::unavailable:
    return "unavailable";
  case ErrCode::io:
    return "io";
  case ErrCode::internal:
    return "internal";
  }
  return "unknown";
}

struct Error {
  ErrCode code;
  std::string detail;
  std::source_location origin;

  static Error make(ErrCode code, std::string detail,
                    std::source_location origin = std::source_location::current()) {
    return Error{code, std::move(detail), origin};
  }
};

template <class T>
using Result = std::expected<T, Error>;

}  // namespace confide::common

#define CFD_CAT_IMPL(a, b) a##b
#define CFD_CAT(a, b) CFD_CAT_IMPL(a, b)

// Propagates a Result<T> failure like Rust's `?`. Usable only inside
// functions returning Result<U>. `var` is bound to the unwrapped value.
#define CFD_TRY(var, expr)                                                 \
  auto&& CFD_CAT(_cfd_r_, __LINE__) = (expr);                              \
  if (!CFD_CAT(_cfd_r_, __LINE__))                                         \
    return std::unexpected(std::move(CFD_CAT(_cfd_r_, __LINE__)).error()); \
  auto var = std::move(*CFD_CAT(_cfd_r_, __LINE__))

// Variant of CFD_TRY for Result<void> expressions where no value is bound.
#define CFD_TRYV(expr)                                                       \
  do {                                                                       \
    auto&& CFD_CAT(_cfd_r_, __LINE__) = (expr);                              \
    if (!CFD_CAT(_cfd_r_, __LINE__))                                         \
      return std::unexpected(std::move(CFD_CAT(_cfd_r_, __LINE__)).error()); \
  } while (0)
