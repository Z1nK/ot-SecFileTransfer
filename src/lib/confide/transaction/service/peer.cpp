#include "common/name.hpp"
#include "transaction/service/detail.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace confide::transaction {

using namespace detail;  // NOLINT(google-build-using-namespace)

Result<Transaction> TransactionService::receive(const Caller& peer, const ReceiveRequest& req) {
  if (!peer.is_peer) {
    return fail(ErrCode::forbidden, "only peers can push transactions");
  }
  if (req.sender.empty() || req.target_user.empty()) {
    return fail(ErrCode::validation, "sender and target_user are required");
  }
  if (req.expires_at <= req.created_at) {
    return fail(ErrCode::validation, "expires_at must be after created_at");
  }
  // The peer is trusted to say who sent it, not to put anything it likes
  // into inboxes, listings and logs: same rule as for our own user names.
  if (!common::is_valid_name(req.sender)) {
    return fail(ErrCode::validation, "sender must be 1-64 chars of [A-Za-z0-9._-]");
  }
  CFD_TRYV(check_comment(req.comment, options_.max_comment_length));
  std::string target_user = req.target_user;
  if (options_.check_destination) {
    CFD_TRY(canonical, options_.check_destination(req.target_user, std::string_view{}));
    target_user = std::move(canonical);
  }
  // Our retention limit holds for what peers store here too. Shortened, not
  // refused: the sending server may simply allow longer than we do.
  const auto expires_at = std::min(
      req.expires_at, clock_.now() + std::chrono::days{options_.max_retention_days});

  const auto mu = lock_for(req.id);
  const std::scoped_lock lock(*mu);

  Transaction tx{
      .id = req.id,
      .sender = req.sender,
      .target_user = std::move(target_user),
      .target_server = {},
      .origin_server = peer.name,
      .comment = req.comment,
      .state = State::open,
      .created_at = req.created_at,
      .expires_at = expires_at,
  };
  if (auto inserted = repo_.insert(tx); !inserted) {
    // `conflict`: the id exists and its mutex stays. After any other failure
    // nothing has this id, so its mutex must not stay behind either.
    if (!repo_.contains(req.id)) {
      const std::scoped_lock locks(locks_mu_);
      locks_.erase(req.id);
    }
    return std::unexpected(std::move(inserted).error());
  }
  return tx;
}

}  // namespace confide::transaction
