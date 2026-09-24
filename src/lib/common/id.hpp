#pragma once
#include "common/error.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace confide::common {

// Transaction id: random UUID v4, canonical lowercase form
// "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx". Used in URLs, as the transaction
// folder name on disk, and kept unchanged when forwarded to a peer,
// so it must be globally unique. Only generate() and parse() create one, so
// every TransactionId holds a valid id (except a moved-from one).
class TransactionId {
public:
  static TransactionId generate();

  static Result<TransactionId> parse(std::string_view str);

  const std::string& str() const noexcept { return value_; }

  auto operator<=>(const TransactionId&) const = default;

private:
  explicit TransactionId(std::string value) : value_(std::move(value)) {}

  std::string value_;
};

}  // namespace confide::common

// Hash specialization for TransactionId to be used in unordered containers.
template <>
struct std::hash<confide::common::TransactionId> {
  std::size_t operator()(const confide::common::TransactionId& id) const noexcept {
    return std::hash<std::string>{}(id.str());
  }
};