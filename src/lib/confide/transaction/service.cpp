#include "transaction/service/detail.hpp"
#include "transaction/state_machine.hpp"

#include <memory>
#include <mutex>

namespace confide::transaction {

using namespace detail;  // NOLINT(google-build-using-namespace)

TransactionService::TransactionService(TransactionRepository& repo, storage::FileStore& store,
                                       logging::BusinessLog& blog, const common::IClock& clock,
                                       ServiceOptions options)
    : repo_(repo), store_(store), blog_(blog), clock_(clock), options_(std::move(options)) {}

std::shared_ptr<std::mutex> TransactionService::lock_for(const common::TransactionId& id) {
  const std::scoped_lock lock(locks_mu_);
  auto& slot = locks_[id];
  if (!slot) {
    slot = std::make_shared<std::mutex>();
  }
  return slot;
}

Result<std::shared_ptr<std::mutex>>
TransactionService::lock_existing(const common::TransactionId& id) {
  if (!repo_.contains(id)) {
    return not_found(id);
  }
  return lock_for(id);
}

std::size_t TransactionService::lock_count() const {
  const std::scoped_lock lock(locks_mu_);
  return locks_.size();
}

Result<void> TransactionService::destroy(
    const common::TransactionId& id, BizEventType type, const Caller& actor,
    const std::function<Result<void>(const Transaction&)>& allowed) {
  {
    CFD_TRY(mu, lock_existing(id));
    const std::scoped_lock lock(*mu);

    auto tx = repo_.find(id);
    if (!tx) {
      return not_found(id);
    }
    if (allowed) {
      CFD_TRYV(allowed(*tx));
    }
    // Already `expired` (an earlier delete failed): just try the delete again.
    if (tx->state != State::expired) {
      CFD_TRYV(check_transition(tx->state, State::expired));
      tx->state = State::expired;
      CFD_TRYV(repo_.save(*tx));
    }
    CFD_TRYV(repo_.remove(id));

    blog_.record(event_for(type, *tx, actor));
  }

  const std::scoped_lock lock(locks_mu_);
  locks_.erase(id);
  return {};
}

}  // namespace confide::transaction
