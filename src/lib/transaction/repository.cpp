#include "transaction/repository.hpp"

#include "transaction/codec.hpp"

#include <format>
#include <utility>

namespace confide::transaction {

namespace {

using common::ErrCode;
using common::Error;

}  // namespace

Result<void> TransactionRepository::load() {
  CFD_TRY(ids, store_.list_transactions());

  std::unordered_map<common::TransactionId, Transaction> loaded;
  loaded.reserve(ids.size());
  for (const auto& id : ids) {
    CFD_TRY(json, store_.read_document(id, storage::Document::meta));
    auto tx = decode_meta(json);
    if (!tx) {
      return std::unexpected(Error::make(
          tx.error().code, std::format("transaction {}: {}", id.str(), tx.error().detail)));
    }
    if (tx->id != id) {
      return std::unexpected(Error::make(
          ErrCode::validation,
          std::format("transaction {}: meta.json holds id {}", id.str(), tx->id.str())));
    }
    loaded.emplace(id, std::move(*tx));
  }

  const std::scoped_lock lock(mu_);
  index_ = std::move(loaded);
  return {};
}

Result<void> TransactionRepository::insert(const Transaction& tx) {
  const std::scoped_lock lock(mu_);
  if (index_.contains(tx.id)) {
    return std::unexpected(
        Error::make(ErrCode::conflict, std::format("transaction {} exists", tx.id.str())));
  }
  CFD_TRYV(store_.create_transaction(tx.id));
  if (auto written = store_.write_document(tx.id, storage::Document::meta, encode_meta(tx));
      !written) {
    // Do not leave a folder without meta.json: load() would fail on it.
    (void)store_.remove_transaction(tx.id);
    return written;
  }
  index_.emplace(tx.id, tx);
  return {};
}

Result<void> TransactionRepository::save(const Transaction& tx) {
  const std::scoped_lock lock(mu_);
  const auto it = index_.find(tx.id);
  if (it == index_.end()) {
    return std::unexpected(
        Error::make(ErrCode::notfound, std::format("no transaction {}", tx.id.str())));
  }
  CFD_TRYV(store_.write_document(tx.id, storage::Document::meta, encode_meta(tx)));
  it->second = tx;
  return {};
}

Result<void> TransactionRepository::save_manifest(const Transaction& tx) {
  return store_.write_document(tx.id, storage::Document::manifest, encode_manifest(tx));
}

Result<void> TransactionRepository::remove(const common::TransactionId& id) {
  const std::scoped_lock lock(mu_);
  CFD_TRYV(store_.remove_transaction(id));
  index_.erase(id);
  return {};
}

std::optional<Transaction> TransactionRepository::find(const common::TransactionId& id) const {
  const std::scoped_lock lock(mu_);
  const auto it = index_.find(id);
  if (it == index_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<Transaction>
TransactionRepository::find_if(const std::function<bool(const Transaction&)>& pred) const {
  const std::scoped_lock lock(mu_);
  std::vector<Transaction> out;
  for (const auto& [id, tx] : index_) {
    if (pred(tx)) {
      out.push_back(tx);
    }
  }
  return out;
}

}  // namespace confide::transaction
