#include "transaction/service/detail.hpp"

#include <algorithm>
#include <mutex>
#include <vector>

namespace confide::transaction {

using namespace detail;  // NOLINT(google-build-using-namespace)

std::vector<Transaction> TransactionService::admin_list(const Caller& admin,
                                                        const AdminFilter& filter) const {
  if (!admin.is_admin) {
    return {};
  }
  auto txs = repo_.find_if([&](const Transaction& tx) {
    return (filter.user.empty() || tx.sender == filter.user || tx.target_user == filter.user)
           && (!filter.state || tx.state == *filter.state)
           && (!filter.from || tx.created_at >= *filter.from)
           && (!filter.to || tx.created_at < *filter.to);
  });
  std::ranges::sort(txs, [](const Transaction& a, const Transaction& b) {
    return a.created_at != b.created_at ? a.created_at > b.created_at : a.id < b.id;
  });
  return txs;
}

Result<Transaction> TransactionService::admin_get(const Caller& admin,
                                                  const common::TransactionId& id) const {
  if (!admin.is_admin) {
    return forbidden(id);
  }
  auto tx = repo_.find(id);
  if (!tx) {
    return not_found(id);
  }
  return std::move(*tx);
}

Result<void> TransactionService::admin_delete(const Caller& admin,
                                              const common::TransactionId& id) {
  if (!admin.is_admin) {
    return forbidden(id);
  }
  return destroy(id, BizEventType::admin_deleted, admin);
}

Result<Transaction> TransactionService::set_retention(const Caller& admin,
                                                      const common::TransactionId& id, int days) {
  if (!admin.is_admin) {
    return forbidden(id);
  }
  if (days < 1 || days > options_.max_retention_days) {
    return fail(ErrCode::validation,
                std::format("days must be 1..{}, got {}", options_.max_retention_days, days));
  }
  const auto mu = lock_for(id);
  const std::scoped_lock lock(*mu);
  auto tx = repo_.find(id);
  if (!tx) {
    return not_found(id);
  }
  if (tx->state == State::expired) {
    return fail(ErrCode::gone, "transaction has expired");
  }
  CFD_TRY(expires, common::expires_at(tx->created_at, days));
  tx->expires_at = expires;
  CFD_TRYV(repo_.save(*tx));

  auto ev = event_for(BizEventType::retention_changed, *tx, admin);
  ev.retention_days = days;
  blog_.record(ev);
  return std::move(*tx);
}

}  // namespace confide::transaction
