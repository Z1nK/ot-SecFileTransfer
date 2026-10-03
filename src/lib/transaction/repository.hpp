#pragma once
#include "storage/storage.hpp"
#include "transaction/transaction.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

// TODO: implement (needs codec).

namespace confide::transaction {

// Persists transactions as meta.json / manifest.json through FileStore and
// keeps all of them in memory, so listing an inbox never scans the disk.
// meta.json is the source of truth: the index is only a cache of it.
//
// Thread-safe for single calls.
class TransactionRepository {
public:
  explicit TransactionRepository(storage::FileStore& store) : store_(store) {}

  TransactionRepository(const TransactionRepository&) = delete;
  TransactionRepository& operator=(const TransactionRepository&) = delete;
  TransactionRepository(TransactionRepository&&) = delete;
  TransactionRepository& operator=(TransactionRepository&&) = delete;
  ~TransactionRepository() = default;

  // Reads meta.json of every transaction folder into the index. Fails on the
  // first unreadable or invalid meta.json, so a damaged store is noticed at
  // startup instead of transactions silently disappearing.
  Result<void> load();

  // Creates the folder and writes meta.json. `conflict` if the id exists.
  Result<void> insert(const Transaction& tx);

  // Rewrites meta.json, then updates the index. `notfound` if unknown.
  Result<void> save(const Transaction& tx);

  // Writes manifest.json from tx.files (at commit).
  Result<void> save_manifest(const Transaction& tx);

  // Deletes the folder with all files, then drops it from the index.
  Result<void> remove(const common::TransactionId& id);

  std::optional<Transaction> find(const common::TransactionId& id) const;

  // Copies of all transactions matching `pred`, e.g. an inbox or the expired
  // ones for the retention job.
  std::vector<Transaction> find_if(const std::function<bool(const Transaction&)>& pred) const;

private:
  storage::FileStore& store_;
  mutable std::mutex mu_;
  std::unordered_map<common::TransactionId, Transaction> index_;
};

}  // namespace confide::transaction
