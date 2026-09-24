#pragma once

#include "common/error.hpp"

namespace confide::common {

// Transaction id: random UUID v4
// "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx". Used in URLs, as the transaction
// folder name on disk.
// so it must be globally unique.
class TransactionId {
public:
  TransactionId() = default;
  TransactionId(const TransactionId&) = delete;
  TransactionId& operator=(const TransactionId&) = delete;
  TransactionId(TransactionId&&) = default;
  TransactionId& operator=(TransactionId&&) = default;
  ~TransactionId() = default;

  static TransactionId generate();

  static Result<TransactionId> parse(std::string_view str);
  auto operator<=>(const TransactionId&) const = default;

private:
  explicit TransactionId(std::string value) : value_(std::move(value)) {}

  std::string value_;
};


}  // namespace confide::common