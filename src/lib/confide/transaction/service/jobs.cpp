#include "transaction/service/detail.hpp"
#include "transaction/state_machine.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace confide::transaction {

using namespace detail;  // NOLINT(google-build-using-namespace)

std::vector<common::TransactionId> TransactionService::list_outgoing() const {
  auto txs = repo_.find_if([](const Transaction& tx) { return is_outgoing(tx); });
  std::ranges::sort(txs, [](const Transaction& a, const Transaction& b) {
    return a.committed_at != b.committed_at ? a.committed_at < b.committed_at : a.id < b.id;
  });
  std::vector<common::TransactionId> ids;
  ids.reserve(txs.size());
  for (const auto& tx : txs) {
    ids.push_back(tx.id);
  }
  return ids;
}

Result<Transaction> TransactionService::outgoing(const common::TransactionId& id) const {
  auto tx = repo_.find(id);
  if (!tx) {
    return not_found(id);
  }
  if (!is_outgoing(*tx)) {
    return fail(ErrCode::conflict,
                std::format("transaction {} is {}, not waiting to be forwarded", id.str(),
                            to_string(tx->state)));
  }
  return std::move(*tx);
}

Result<void> TransactionService::mark_delivered(const common::TransactionId& id) {
  CFD_TRY(mu, lock_existing(id));
  const std::scoped_lock lock(*mu);

  auto tx = repo_.find(id);
  if (!tx) {
    return not_found(id);
  }
  CFD_TRYV(check_transition(tx->state, State::delivered));
  tx->state = State::delivered;
  tx->delivered_at = clock_.now();
  tx->forwarding.reset();
  CFD_TRYV(repo_.save(*tx));

  auto ev = event_for(BizEventType::forwarded, *tx, Caller{.name = "system", .is_peer = false});
  ev.file_count = static_cast<std::uint32_t>(tx->files.size());
  ev.total_bytes = tx->total_bytes();
  blog_.record(ev);
  return {};
}

Result<void> TransactionService::record_forward_failure(
    const common::TransactionId& id, const common::Error& err,
    std::optional<std::chrono::milliseconds> retry_in) {
  auto mu = lock_existing(id);
  if (!mu) {
    return {};  // deleted meanwhile
  }
  const std::scoped_lock lock(**mu);

  auto tx = repo_.find(id);
  if (!tx || !is_outgoing(*tx)) {
    return {};
  }
  const auto now = clock_.now();
  auto& fwd = tx->forwarding ? *tx->forwarding : tx->forwarding.emplace();
  fwd.attempts += 1;
  fwd.last_attempt_at = now;
  fwd.error = std::string{common::to_string(err.code)};
  if (retry_in) {
    fwd.status = ForwardStatus::retrying;
    fwd.next_attempt_at = now + std::chrono::duration_cast<common::Timestamp::duration>(*retry_in);
  } else {
    fwd.status = ForwardStatus::failed;
    fwd.next_attempt_at.reset();
  }
  CFD_TRYV(repo_.save(*tx));

  if (!retry_in) {
    auto ev = event_for(BizEventType::forward_failed, *tx, Caller{.name = "system", .is_peer = false});
    ev.error = fwd.error;
    ev.detail = err.detail;  // for the administrator; not kept on the transaction
    blog_.record(ev);
  }
  return {};
}

std::vector<common::TransactionId> TransactionService::list_expired() const {
  const auto now = clock_.now();
  const auto txs =
      repo_.find_if([&](const Transaction& tx) { return common::is_expired(tx.expires_at, now); });
  std::vector<common::TransactionId> ids;
  ids.reserve(txs.size());
  for (const auto& tx : txs) {
    ids.push_back(tx.id);
  }
  return ids;
}

Result<void> TransactionService::expire(const common::TransactionId& id) {
  return destroy(id, BizEventType::expired, Caller{.name = "system", .is_peer = false});
}

}  // namespace confide::transaction
