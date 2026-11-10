#pragma once
// Plain data passed to TransactionService: who calls, what they ask for, and
// the options `main` wires in. Included by service.hpp.
#include "common/id.hpp"
#include "common/time.hpp"
#include "transaction/transaction.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace confide::transaction {

// Who is calling. Filled by `api` from auth's principal, so this module does
// not depend on `auth`.
struct Caller {
  std::string name;       // user name, or the peer's server name
  bool is_peer = false;   // authenticated with the peer token (/internal/*)
  bool is_admin = false;  // member of auth.admin_group on an /admin/* route
};

struct CreateRequest {
  std::string target_user;
  std::string target_server;  // empty or this server's name = local target
  int retention_days = 0;     // 0 = ServiceOptions::default_retention_days
  std::string comment{};      // optional, see ServiceOptions::max_comment_length
};

// A transaction pushed by a peer (Architecture §7). Keeps the sender's id
// and times; the new transaction starts `open` until the peer commits it.
struct ReceiveRequest {
  common::TransactionId id;
  std::string sender;
  std::string target_user;
  common::Timestamp created_at;
  common::Timestamp expires_at;
  std::string comment{};
};

// Which transactions TransactionService::admin_list() returns; empty or
// nullopt fields match everything.
struct AdminFilter {
  std::string user{};  // sender or target user
  std::optional<State> state{};
  std::optional<common::Timestamp> from{};  // created_at >= from
  std::optional<common::Timestamp> to{};    // created_at < to
};

enum class Box : std::uint8_t {
  in,   // transactions where the caller is the target user
  out,  // transactions the caller sent
};

// `unknown_destination` if the user or server is not known. Wired by
// `main` to auth's UserStore (local target) and PeerRegistry (remote).
using DestinationCheck =
    std::function<Result<void>(std::string_view target_user, std::string_view target_server)>;

// True if `user` is a member of the group named `target`. Lets group
// members read transactions addressed to the group. Wired by `main` to the
// configured groups, so this module does not depend on `config`.
using MembershipCheck = std::function<bool(std::string_view user, std::string_view target)>;

// Called after a successful commit, outside the transaction lock. `main`
// wires it to forwarder::Forwarder::enqueue for remote targets, so this
// module does not depend on `forwarder`.
using CommitHook = std::function<void(const Transaction&)>;

struct ServiceOptions {
  std::string local_server;  // this server's name, from config
  int default_retention_days = 30;
  int max_retention_days = 365;  // upper limit for CreateRequest::retention_days
  std::size_t max_comment_length = kMaxCommentLength;  // in code points
  DestinationCheck check_destination;
  MembershipCheck is_recipient;  // may be empty: only exact name matches
  CommitHook on_committed;  // may be empty
};

}  // namespace confide::transaction
