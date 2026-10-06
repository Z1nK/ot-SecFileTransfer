#include "transaction/transaction.hpp"

#include <algorithm>
#include <numeric>
#include <string>

namespace confide::transaction {

std::string_view to_string(State state) {
  switch (state) {
  case State::open:
    return "open";
  case State::committed:
    return "committed";
  case State::delivered:
    return "delivered";
  case State::expired:
    return "expired";
  }
  return "unknown";
}

Result<State> parse_state(std::string_view text) {
  for (const auto s : {State::open, State::committed, State::delivered, State::expired}) {
    if (text == to_string(s)) {
      return s;
    }
  }
  return std::unexpected(
      common::Error::make(common::ErrCode::validation, "unknown state: " + std::string{text}));
}

const FileEntry* Transaction::find_file(const common::RelativePath& path) const {
  const auto it = std::ranges::lower_bound(files, path, {}, &FileEntry::path);
  return (it != files.end() && it->path == path) ? &*it : nullptr;
}

std::uint64_t Transaction::total_bytes() const {
  return std::accumulate(files.begin(), files.end(), std::uint64_t{0},
                         [](std::uint64_t sum, const FileEntry& f) { return sum + f.size; });
}

}  // namespace confide::transaction
