#pragma once
#include "common/error.hpp"
#include "common/id.hpp"
#include "common/path_sanitizer.hpp"
#include "common/sha256.hpp"
#include "common/time.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace confide::transaction {

using common::Result;

// Lifecycle of a transaction (Architecture §5). Rules for moving between
// states live in state_machine.hpp.
enum class State : std::uint8_t {
  open,       // accepts file uploads and deletes
  committed,  // immutable; downloadable when the target is local
  delivered,  // forwarded to the target server (sender side only)
  expired,    // retention reached, waiting for the retention job to delete it
};

// Stable lowercase name, as stored in meta.json and returned by the API.
std::string_view to_string(State state);

// `validation` for anything to_string() does not produce.
Result<State> parse_state(std::string_view text);

// A payload file as recorded when its upload finished. The digest is checked
// again against the bytes on disk at commit
struct FileEntry {
  common::RelativePath path;
  std::uint64_t size = 0;
  common::Sha256Digest sha256;

  bool operator==(const FileEntry&) const = default;
};

// Everything meta.json holds about one transaction. Plain data: the rules
// for changing it are in TransactionService.
struct Transaction {
  common::TransactionId id;
  std::string sender;         // user name on the origin server
  std::string target_user;
  std::string target_server;  // empty = target user is on this server
  std::string origin_server;  // empty = created here; else the peer that pushed it
  State state = State::open;
  common::Timestamp created_at;
  common::Timestamp expires_at;
  std::optional<common::Timestamp> committed_at{};
  std::optional<common::Timestamp> delivered_at{};
  std::vector<FileEntry> files{};  // sorted by path

  // nullptr if the transaction has no file with this path.
  const FileEntry* find_file(const common::RelativePath& path) const;

  std::uint64_t total_bytes() const;

  bool operator==(const Transaction&) const = default;
};

}  // namespace confide::transaction
