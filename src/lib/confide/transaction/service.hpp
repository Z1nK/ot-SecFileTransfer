#pragma once
#include "common/time.hpp"
#include "logging/business_log.hpp"
#include "storage/storage.hpp"
#include "transaction/repository.hpp"
#include "transaction/service_types.hpp"
#include "transaction/transaction.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace confide::transaction {

class TransactionService;

// An upload in progress, returned by TransactionService::begin_upload().
// Bytes are streamed without holding the transaction lock; finish() takes
// the lock, checks that the transaction is still open, and only then moves
// the file into place. If the transaction was committed in the meantime,
// finish() returns `conflict` and the temp file is dropped.
class PendingUpload {
public:
  Result<void> write(common::ByteSpan chunk) { return upload_.write(chunk); }
  Result<void> write(std::string_view chunk) { return upload_.write(chunk); }

  std::uint64_t size() const noexcept { return upload_.size(); }

  // `integrity` if `expected` (X-Checksum-SHA256) does not match, `conflict`
  // or `gone` if the transaction is no longer open, plus storage errors.
  // Logs `file_added` on success.
  Result<FileEntry> finish(const std::optional<common::Sha256Digest>& expected = std::nullopt);

private:
  friend class TransactionService;

  PendingUpload(TransactionService& service, common::TransactionId id, Caller caller,
                storage::Upload upload)
      : service_(&service), id_(std::move(id)), caller_(std::move(caller)),
        upload_(std::move(upload)) {}

  TransactionService* service_;
  common::TransactionId id_;
  Caller caller_;
  storage::Upload upload_;
};

// Business rules of transactions (FR-1..4, FR-12, FR-13): state machine,
// access rules, business logging. Knows nothing about HTTP.
//
// Members are split by section: service.cpp (locks, destroy),
// service/sender.cpp, service/reading.cpp, service/peer.cpp,
// service/admin.cpp, service/jobs.cpp (forwarder and retention).
//
// Access rules:
//   - sender:      everything on own transactions while open; read after
//   - target user: list and read once committed or delivered; for a group
//                  target, every member of the group (MembershipCheck)
//   - peer:        receive / add files / commit on transactions it pushed
//   - admin:       metadata of every transaction, delete, change retention;
//                  never file contents
//   - anyone else: `forbidden` (api may turn that into 404)
//
// Thread-safe. One mutex per transaction protects its state machine;
// different transactions never block each other (Architecture §10).
class TransactionService {
public:
  TransactionService(TransactionRepository& repo, storage::FileStore& store,
                     logging::BusinessLog& blog, const common::IClock& clock,
                     ServiceOptions options);

  // Not copyable or movable: owns mutexes, and every PendingUpload points
  // back to it.
  TransactionService(const TransactionService&) = delete;
  TransactionService& operator=(const TransactionService&) = delete;
  TransactionService(TransactionService&&) = delete;
  TransactionService& operator=(TransactionService&&) = delete;
  ~TransactionService() = default;

  // --- Sender -------------------------------------------------------------

  // `validation` on bad retention_days or comment, `unknown_destination`
  // (FR-13).
  Result<Transaction> create(const Caller& caller, const CreateRequest& req);

  Result<PendingUpload> begin_upload(const Caller& caller, const common::TransactionId& id,
                                     const common::RelativePath& path);

  Result<void> remove_file(const Caller& caller, const common::TransactionId& id,
                           const common::RelativePath& path);

  // Re-hashes every file against its FileEntry (`integrity` on mismatch),
  // writes manifest.json, then meta.json with state=committed. A crash
  // between the two leaves the transaction open with a stray manifest,
  // which the next commit overwrites.
  Result<Transaction> commit(const Caller& caller, const common::TransactionId& id);

  // Deletes an open transaction of the caller with its files and logs
  // `discarded`: a send that failed half way, or one made by mistake. Only
  // the sender, and only while it is open: `conflict` once committed (what
  // the target may already have seen stays until retention or an admin
  // removes it), `gone` if expired. An upload in progress fails at its end.
  Result<void> discard(const Caller& caller, const common::TransactionId& id);

  // Asks for another attempt to forward a committed transaction of the
  // caller to its target server: after a `failed` push once the cause is
  // fixed, or to cut a `retrying` wait short. Sets the status to `pending`,
  // next_attempt_at to now or, if later, retry_min_interval after the last
  // failed attempt, and calls ServiceOptions::on_retry. Asking again while
  // it is `pending` changes nothing. Only the sender; `conflict` if the
  // transaction is not waiting to be forwarded (open, local, delivered),
  // `gone` if expired.
  Result<Transaction> retry_forward(const Caller& caller, const common::TransactionId& id);

  // --- Reading ------------------------------------------------------------

  Result<Transaction> get(const Caller& caller, const common::TransactionId& id) const;

  std::vector<Transaction> list(const Caller& caller, Box box) const;

  Result<storage::Download> open_download(const Caller& caller, const common::TransactionId& id,
                                          const common::RelativePath& path) const;

  // --- Peer (receiving side) ----------------------------------------------

  // `conflict` if the id exists, `unknown_destination` if target_user is not
  // a local user. Followed by begin_upload() and commit() as the peer.
  Result<Transaction> receive(const Caller& peer, const ReceiveRequest& req);

  // --- Admin --------------------------------------------------------------
  // All `forbidden` unless caller.is_admin.

  // Newest first. Empty for a non-admin.
  std::vector<Transaction> admin_list(const Caller& admin, const AdminFilter& filter) const;

  Result<Transaction> admin_get(const Caller& admin, const common::TransactionId& id) const;

  // Deletes the transaction with its files, in any state, and logs
  // `admin_deleted`. A forward in progress fails and is not retried.
  Result<void> admin_delete(const Caller& admin, const common::TransactionId& id);

  // expires_at = created_at + `days` (1..max_retention_days); a date in the
  // past leaves it to the next retention sweep. Logs `retention_changed`.
  // `gone` for an expired transaction.
  Result<Transaction> set_retention(const Caller& admin, const common::TransactionId& id, int days);

  // --- Forwarder and retention --------------------------------------------

  // Committed transactions created here for a user on another server, i.e.
  // the ones still to be forwarded. Oldest first.
  std::vector<common::TransactionId> list_outgoing() const;

  // One of list_outgoing(), with its files. `notfound` if it does not exist,
  // `conflict` if it is not a committed transaction for another server (not
  // committed yet, already delivered, expired, or local).
  Result<Transaction> outgoing(const common::TransactionId& id) const;

  // committed -> delivered, after the peer confirmed the push. Clears
  // Transaction::forwarding.
  Result<void> mark_delivered(const common::TransactionId& id);

  // Records a failed push in Transaction::forwarding, which the sender
  // sees (the error code; err.detail only goes to the business log).
  // `retry_in` set: the forwarder tries again after it (`retrying`);
  // nullopt: it gave up until a retry or the next start (`failed`, also
  // logged as `forward_failed`). Does nothing for a transaction that is no
  // longer waiting to be forwarded (deleted, delivered, expired).
  Result<void> record_forward_failure(const common::TransactionId& id, const common::Error& err,
                                      std::optional<std::chrono::milliseconds> retry_in);

  // Transactions whose expires_at has passed, in any state.
  std::vector<common::TransactionId> list_expired() const;

  // Marks the transaction expired, deletes it with its files and logs
  // `expired`.
  Result<void> expire(const common::TransactionId& id);

  // Number of per-transaction mutexes held, for tests.
  std::size_t lock_count() const;

private:
  friend class PendingUpload;

  // Mutex of one transaction, created on first use.
  std::shared_ptr<std::mutex> lock_for(const common::TransactionId& id);

  // lock_for() of a transaction that exists, `notfound` otherwise. Every call
  // on an id from a request goes through here: lock_for() alone would keep
  // a mutex for each id anyone ever asked about, existing or not.
  Result<std::shared_ptr<std::mutex>> lock_existing(const common::TransactionId& id);

  // Marks the transaction expired, deletes it with its files and logs
  // `type` with `actor`. Shared by expire(), admin_delete() and discard().
  // `allowed`, if set, is asked under the transaction's lock and its error
  // ends the call: the state it saw can't change before the delete.
  Result<void> destroy(const common::TransactionId& id, logging::BizEventType type,
                       const Caller& actor,
                       const std::function<Result<void>(const Transaction&)>& allowed = {});

  TransactionRepository& repo_;
  storage::FileStore& store_;
  logging::BusinessLog& blog_;
  const common::IClock& clock_;
  ServiceOptions options_;

  mutable std::mutex locks_mu_;
  std::unordered_map<common::TransactionId, std::shared_ptr<std::mutex>> locks_;
};

}  // namespace confide::transaction
