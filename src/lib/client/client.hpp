#pragma once
// Shared by the client library sources: error and URL helpers. Nothing here
// talks to a terminal, so any front end (ftc, a GUI, ...) can use it.
#include <common/error.hpp>
#include <common/id.hpp>

#include <string>
#include <utility>

namespace confide::client {

using common::ErrCode;
using common::Error;
using common::Result;

inline std::unexpected<Error> fail(ErrCode code, std::string detail) {
  return std::unexpected(Error::make(code, std::move(detail)));
}

// "/transactions/{id}", the base of every per-transaction request.
inline std::string tx_target(const common::TransactionId& id) {
  return "/transactions/" + id.str();
}

}  // namespace confide::client
