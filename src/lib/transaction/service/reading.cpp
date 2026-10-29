#include "transaction/service/detail.hpp"
#include "transaction/state_machine.hpp"

#include <algorithm>
#include <vector>

namespace confide::transaction {

using namespace detail;  // NOLINT(google-build-using-namespace)

Result<Transaction> TransactionService::get(const Caller& caller,
                                            const common::TransactionId& id) const {
  auto tx = repo_.find(id);
  if (!tx) {
    return not_found(id);
  }
  if (!may_read(caller, *tx, options_.is_recipient)) {
    return forbidden(id);
  }
  return std::move(*tx);
}

std::vector<Transaction> TransactionService::list(const Caller& caller, Box box) const {
  if (caller.is_peer) {
    return {};
  }
  auto txs = repo_.find_if([&](const Transaction& tx) {
    if (box == Box::out) {
      return is_sender(caller, tx);
    }
    return is_target(caller, tx, options_.is_recipient)
           && (tx.state == State::committed || tx.state == State::delivered);
  });
  std::ranges::sort(txs, [](const Transaction& a, const Transaction& b) {
    return a.created_at != b.created_at ? a.created_at > b.created_at : a.id < b.id;
  });
  return txs;
}

Result<storage::Download> TransactionService::open_download(const Caller& caller,
                                                            const common::TransactionId& id,
                                                            const common::RelativePath& path) const {
  const auto tx = repo_.find(id);
  if (!tx) {
    return not_found(id);
  }
  if (!may_read(caller, *tx, options_.is_recipient)) {
    return forbidden(id);
  }
  CFD_TRYV(require_readable(tx->state));
  if (tx->find_file(path) == nullptr) {
    return fail(ErrCode::notfound, "no such file: " + path.str());
  }
  return store_.open_read(id, path);
}

}  // namespace confide::transaction
