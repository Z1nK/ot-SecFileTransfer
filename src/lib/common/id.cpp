#include "common/id.hpp"

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <cstddef>

namespace confide::common {

namespace {

constexpr std::size_t kUuidTextLen = 36;  // 32 hex digits + 4 dashes
constexpr std::size_t kVersionPos = 14;   // "xxxxxxxx-xxxx-Vxxx-..."
constexpr std::size_t kVariantPos = 19;   // "xxxxxxxx-xxxx-xxxx-Nxxx-..."

bool is_dash_pos(std::size_t pos) {
  return pos == 8 || pos == 13 || pos == 18 || pos == 23;
}

bool is_lower_hex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

}  // namespace

TransactionId TransactionId::generate() {
  thread_local boost::uuids::random_generator gen;
  return TransactionId{boost::uuids::to_string(gen())};
}

Result<TransactionId> TransactionId::parse(std::string_view str) {
  auto fail = [] {
    return std::unexpected(
        Error::make(ErrCode::validation, "malformed transaction id"));
  };

  if (str.size() != kUuidTextLen) {
    return fail();
  }
  for (std::size_t i = 0; i < str.size(); ++i) {
    const bool ok = is_dash_pos(i) ? str[i] == '-' : is_lower_hex(str[i]);
    if (!ok) {
      return fail();
    }
  }
  const char variant = str[kVariantPos];
  if (str[kVersionPos] != '4' ||
      (variant != '8' && variant != '9' && variant != 'a' && variant != 'b')) {
    return fail();
  }
  return TransactionId{std::string{str}};
}

}  // namespace confide::common
