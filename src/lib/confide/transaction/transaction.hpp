#pragma once
#include "common/error.hpp"
#include "common/id.hpp"
#include "common/path_sanitizer.hpp"
#include "common/sha256.hpp"
#include "common/time.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace confide::transaction {

using common::Result;

// Default upper bound for Transaction::comment, in Unicode code points
// (not bytes). ServiceOptions::max_comment_length overrides it.
inline constexpr std::size_t kMaxCommentLength = 256;

// `validation` if `comment` is longer than `max_length` code points or holds
// control characters (newline, tab, ...): it is shown on one line in lists.
// `comment` must be valid UTF-8, as Boost.JSON guarantees for parsed input.
Result<void> check_comment(std::string_view comment, std::size_t max_length);

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

// How forwarding to the target server goes, for a committed transaction
// created here for a user on another server. Not a State: the transaction
// stays `committed` until a push succeeds, whatever happened before.
enum class ForwardStatus : std::uint8_t {
  pending,   // not tried yet, or a retry was asked for
  retrying,  // the last attempt failed with an error that may go away
  failed,    // refused for good; waits for `ftc retry` or the next server start
};

std::string_view to_string(ForwardStatus status);

// `validation` for anything to_string() does not produce.
Result<ForwardStatus> parse_forward_status(std::string_view text);

// Outcome of the forwarder's attempts so far. Only the forwarder (through
// TransactionService) writes it; cleared when the push succeeds. It holds
// the error code only: what the peer or the network said goes to the logs,
// not into what the sender can read.
struct ForwardInfo {
  ForwardStatus status = ForwardStatus::pending;
  std::uint32_t attempts = 0;  // failed attempts, across server restarts
  std::optional<common::Timestamp> last_attempt_at{};
  std::optional<common::Timestamp> next_attempt_at{};  // when `retrying`
  std::string error{};  // ErrCode name of the last failure

  bool operator==(const ForwardInfo&) const = default;
};

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
  std::string comment{};      // free text from the sender, may be empty
  State state = State::open;
  common::Timestamp created_at;
  common::Timestamp expires_at;
  std::optional<common::Timestamp> committed_at{};
  std::optional<common::Timestamp> delivered_at{};
  std::vector<FileEntry> files{};  // sorted by path
  // Sender side of a remote target, once an attempt failed or a retry was
  // asked for. nullopt = nothing to report (not tried yet, or delivered).
  std::optional<ForwardInfo> forwarding{};

  // nullptr if the transaction has no file with this path.
  const FileEntry* find_file(const common::RelativePath& path) const;

  std::uint64_t total_bytes() const;

  bool operator==(const Transaction&) const = default;
};

}  // namespace confide::transaction
