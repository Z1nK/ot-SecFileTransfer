#include "transaction/service/detail.hpp"
#include "transaction/state_machine.hpp"

#include <algorithm>
#include <functional>
#include <set>
#include <string>
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
  std::vector<Transaction> txs;
  if (box == Box::out) {
    txs = repo_.find_if([&](const Transaction& tx) { return is_sender(caller, tx); });
  } else {
    // What an inbox can show at all, whoever asks.
    const auto listed = [](const Transaction& tx) {
      return tx.target_server.empty()
             && (tx.state == State::committed || tx.state == State::delivered);
    };
    // The membership check may ask the user directory over the network
    // (LDAP), and find_if() holds the repository lock while it runs the
    // predicate. So: collect the target names under the lock, ask about each
    // one once without it, then pick the transactions by name.
    std::set<std::string, std::less<>> targets;
    repo_.find_if([&](const Transaction& tx) {
      if (listed(tx)) {
        targets.insert(tx.target_user);
      }
      return false;
    });
    std::erase_if(targets, [&](const std::string& target) {
      return caller.name != target
             && !(options_.is_recipient && options_.is_recipient(caller.name, target));
    });
    txs = repo_.find_if(
        [&](const Transaction& tx) { return listed(tx) && targets.contains(tx.target_user); });
  }
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
