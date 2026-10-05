#pragma once
#include "common/time.hpp"
#include "logging/business_log.hpp"
#include "storage/storage.hpp"
#include "transaction/repository.hpp"
#include "transaction/transaction.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace confide::transaction {

// Who is calling. Filled by `api` from auth's principal, so this module does
// not depend on `auth`.
struct Caller {
  std::string name;      // user name, or the peer's server name
  bool is_peer = false;  // authenticated with the peer token (/internal/*)
};

struct CreateRequest {
  std::string target_user;
  std::string target_server;  // empty or this server's name = local target
  int retention_days = 0;     // 0 = ServiceOptions::default_retention_days
};

// A transaction pushed by a peer (Architecture §7). Keeps the sender's id
// and times; the new transaction starts `open` until the peer commits it.
struct ReceiveRequest {
  common::TransactionId id;
  std::string sender;
  std::string target_user;
  common::Timestamp created_at;
  common::Timestamp expires_at;
};

enum class Box : std::uint8_t {
  in,   // transactions where the caller is the target user
  out,  // transactions the caller sent
};

// `unknown_destination` if the user or server is not known. Wired by
// `main` to auth's UserStore (local target) and PeerRegistry (remote).
using DestinationCheck =
    std::function<Result<void>(std::string_view target_user, std::string_view target_server)>;

// Called after a successful commit, outside the transaction lock. `main`
// wires it to ForwardQueue::enqueue for remote targets, so this module does
// not depend on `forwarder`.
using CommitHook = std::function<void(const Transaction&)>;

struct ServiceOptions {
  std::string local_server;  // this server's name, from config
  int default_retention_days = 30;
  DestinationCheck check_destination;
  CommitHook on_committed;  // may be empty
};

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
// Access rules:
//   - sender:      everything on own transactions while open; read after
//   - target user: list and read once committed or delivered
//   - peer:        receive / add files / commit on transactions it pushed
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

  // `validation` on bad retention_days, `unknown_destination` (FR-13).
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

  // --- Reading ------------------------------------------------------------

  Result<Transaction> get(const Caller& caller, const common::TransactionId& id) const;

  std::vector<Transaction> list(const Caller& caller, Box box) const;

  Result<storage::Download> open_download(const Caller& caller, const common::TransactionId& id,
                                          const common::RelativePath& path) const;

  // --- Peer (receiving side) ----------------------------------------------

  // `conflict` if the id exists, `unknown_destination` if target_user is not
  // a local user. Followed by begin_upload() and commit() as the peer.
  Result<Transaction> receive(const Caller& peer, const ReceiveRequest& req);

  // --- Forwarder and retention --------------------------------------------

  // committed -> delivered, after the peer confirmed the push.
  Result<void> mark_delivered(const common::TransactionId& id);

  // Transactions whose expires_at has passed, in any state.
  std::vector<common::TransactionId> list_expired() const;

  // Marks the transaction expired, deletes it with its files and logs
  // `expired`.
  Result<void> expire(const common::TransactionId& id);

private:
  friend class PendingUpload;

  // Mutex of one transaction, created on first use.
  std::shared_ptr<std::mutex> lock_for(const common::TransactionId& id);

  TransactionRepository& repo_;
  storage::FileStore& store_;
  logging::BusinessLog& blog_;
  const common::IClock& clock_;
  ServiceOptions options_;

  std::mutex locks_mu_;
  std::unordered_map<common::TransactionId, std::shared_ptr<std::mutex>> locks_;
};

}  // namespace confide::transaction
