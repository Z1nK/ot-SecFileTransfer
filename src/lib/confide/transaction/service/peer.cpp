#include "transaction/service/detail.hpp"

#include <mutex>
#include <string_view>

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
  CFD_TRYV(check_comment(req.comment, options_.max_comment_length));
  if (options_.check_destination) {
    CFD_TRYV(options_.check_destination(req.target_user, std::string_view{}));
  }

  const auto mu = lock_for(req.id);
  const std::scoped_lock lock(*mu);

  Transaction tx{
      .id = req.id,
      .sender = req.sender,
      .target_user = req.target_user,
      .target_server = {},
      .origin_server = peer.name,
      .comment = req.comment,
      .state = State::open,
      .created_at = req.created_at,
      .expires_at = req.expires_at,
  };
  CFD_TRYV(repo_.insert(tx));  // `conflict` if the id exists
  return tx;
}

}  // namespace confide::transaction
