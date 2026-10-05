#include "transaction/service.hpp"

#include "transaction/state_machine.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace confide::transaction {

namespace {

using common::ErrCode;
using common::Error;
using logging::BizEvent;
using logging::BizEventType;

std::unexpected<Error> fail(ErrCode code, std::string detail) {
  return std::unexpected(Error::make(code, std::move(detail)));
}

std::unexpected<Error> not_found(const common::TransactionId& id) {
  return fail(ErrCode::notfound, std::format("no transaction {}", id.str()));
}

std::unexpected<Error> forbidden(const common::TransactionId& id) {
  return fail(ErrCode::forbidden, std::format("no access to transaction {}", id.str()));
}

// "alice", or "peer:<server>" for a peer, as the business log expects.
std::string actor_of(const Caller& caller) {
  return caller.is_peer ? "peer:" + caller.name : caller.name;
}

BizEvent event_for(BizEventType type, const Transaction& tx, const Caller& caller) {
  return BizEvent{
      .type = type,
      .tx = tx.id,
      .actor = actor_of(caller),
      .from_user = tx.sender,
      .to_user = tx.target_user,
      .to_server = tx.target_server,
  };
}

// The user who created the transaction on this server.
bool is_sender(const Caller& caller, const Transaction& tx) {
  return !caller.is_peer && tx.origin_server.empty() && caller.name == tx.sender;
}

// The user the transaction is addressed to, on this server.
bool is_target(const Caller& caller, const Transaction& tx) {
  return !caller.is_peer && tx.target_server.empty() && caller.name == tx.target_user;
}

// The peer that pushed the transaction here.
bool is_origin_peer(const Caller& caller, const Transaction& tx) {
  return caller.is_peer && !tx.origin_server.empty() && caller.name == tx.origin_server;
}

// Who may add or delete files and commit: the sender, or the peer that pushed it.
bool may_write(const Caller& caller, const Transaction& tx) {
  return is_sender(caller, tx) || is_origin_peer(caller, tx);
}

// Who may see metadata and files: the sender always, the target once the
// sender has committed, the pushing peer.
bool may_read(const Caller& caller, const Transaction& tx) {
  return is_sender(caller, tx) || is_origin_peer(caller, tx) ||
         (is_target(caller, tx) && tx.state != State::open);
}

void upsert_file(std::vector<FileEntry>& files, FileEntry entry) {
  const auto it = std::ranges::lower_bound(files, entry.path, {}, &FileEntry::path);
  if (it != files.end() && it->path == entry.path) {
    *it = std::move(entry);
  } else {
    files.insert(it, std::move(entry));
  }
}

}  // namespace

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

// --- Sender -----------------------------------------------------------------

Result<Transaction> TransactionService::create(const Caller& caller, const CreateRequest& req) {
  if (caller.is_peer) {
    return fail(ErrCode::forbidden, "peers cannot create transactions");
  }
  if (req.target_user.empty()) {
    return fail(ErrCode::validation, "target_user is empty");
  }
  if (req.retention_days < 0) {
    return fail(ErrCode::validation, "retention_days must not be negative");
  }

  const bool local_target = req.target_server.empty() || req.target_server == options_.local_server;
  const std::string target_server = local_target ? std::string{} : req.target_server;

  if (options_.check_destination) {
    CFD_TRYV(options_.check_destination(req.target_user, target_server));
  }

  const auto created = clock_.now();
  const int days = req.retention_days == 0 ? options_.default_retention_days : req.retention_days;
  CFD_TRY(expires, common::expires_at(created, days));

  Transaction tx{
      .id = common::TransactionId::generate(),
      .sender = caller.name,
      .target_user = req.target_user,
      .target_server = target_server,
      .origin_server = {},
      .state = State::open,
      .created_at = created,
      .expires_at = expires,
  };
  CFD_TRYV(repo_.insert(tx));

  blog_.record(event_for(BizEventType::tx_created, tx, caller));
  return tx;
}

Result<PendingUpload> TransactionService::begin_upload(const Caller& caller,
                                                       const common::TransactionId& id,
                                                       const common::RelativePath& path) {
  const auto mu = lock_for(id);
  const std::scoped_lock lock(*mu);

  const auto tx = repo_.find(id);
  if (!tx) {
    return not_found(id);
  }
  if (!may_write(caller, *tx)) {
    return forbidden(id);
  }
  CFD_TRYV(require_mutable(tx->state));

  // The lock is released before any bytes arrive; finish() checks again.
  CFD_TRY(upload, store_.begin_upload(id, path));
  return PendingUpload(*this, id, caller, std::move(upload));
}

Result<FileEntry> PendingUpload::finish(const std::optional<common::Sha256Digest>& expected) {
  const auto mu = service_->lock_for(id_);
  const std::scoped_lock lock(*mu);

  auto tx = service_->repo_.find(id_);
  if (!tx) {
    return not_found(id_);
  }
  // Committed or expired while the bytes were arriving: the temp file is
  // dropped with `upload_`.
  CFD_TRYV(require_mutable(tx->state));

  CFD_TRY(stored, upload_.commit(expected));
  FileEntry entry{.path = stored.path, .size = stored.size, .sha256 = stored.sha256};
  upsert_file(tx->files, entry);
  CFD_TRYV(service_->repo_.save(*tx));

  auto ev = event_for(BizEventType::file_added, *tx, caller_);
  ev.file = entry.path.str();
  ev.size = entry.size;
  ev.sha256 = entry.sha256.to_hex();
  service_->blog_.record(ev);
  return entry;
}

Result<void> TransactionService::remove_file(const Caller& caller, const common::TransactionId& id,
                                             const common::RelativePath& path) {
  const auto mu = lock_for(id);
  const std::scoped_lock lock(*mu);

  auto tx = repo_.find(id);
  if (!tx) {
    return not_found(id);
  }
  if (!may_write(caller, *tx)) {
    return forbidden(id);
  }
  CFD_TRYV(require_mutable(tx->state));

  const auto it = std::ranges::find(tx->files, path, &FileEntry::path);
  if (it == tx->files.end()) {
    return fail(ErrCode::notfound, "no such file: " + path.str());
  }
  CFD_TRYV(store_.remove_file(id, path));
  tx->files.erase(it);
  CFD_TRYV(repo_.save(*tx));

  auto ev = event_for(BizEventType::file_removed, *tx, caller);
  ev.file = path.str();
  blog_.record(ev);
  return {};
}

Result<Transaction> TransactionService::commit(const Caller& caller,
                                               const common::TransactionId& id) {
  std::optional<Transaction> committed;
  {
    const auto mu = lock_for(id);
    const std::scoped_lock lock(*mu);

    auto found = repo_.find(id);
    if (!found) {
      return not_found(id);
    }
    auto tx = std::move(*found);
    if (!may_write(caller, tx)) {
      return forbidden(id);
    }
    CFD_TRYV(check_transition(tx.state, State::committed));

    // NFR-6: the bytes on disk must still be the ones that were uploaded.
    for (const auto& entry : tx.files) {
      CFD_TRY(actual, store_.hash_file(id, entry.path));
      if (actual.size != entry.size || actual.sha256 != entry.sha256) {
        return fail(ErrCode::integrity, "file changed on disk: " + entry.path.str());
      }
    }

    tx.state = State::committed;
    tx.committed_at = clock_.now();
    CFD_TRYV(repo_.save_manifest(tx));
    CFD_TRYV(repo_.save(tx));

    const bool pushed = is_origin_peer(caller, tx);
    auto ev = event_for(pushed ? BizEventType::received : BizEventType::committed, tx, caller);
    ev.file_count = static_cast<std::uint32_t>(tx.files.size());
    ev.total_bytes = tx.total_bytes();
    blog_.record(ev);
    committed = std::move(tx);
  }

  if (options_.on_committed) {
    options_.on_committed(*committed);
  }
  return std::move(*committed);
}

// --- Reading ----------------------------------------------------------------

Result<Transaction> TransactionService::get(const Caller& caller,
                                            const common::TransactionId& id) const {
  auto tx = repo_.find(id);
  if (!tx) {
    return not_found(id);
  }
  if (!may_read(caller, *tx)) {
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
    return is_target(caller, tx) && (tx.state == State::committed || tx.state == State::delivered);
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
  if (!may_read(caller, *tx)) {
    return forbidden(id);
  }
  CFD_TRYV(require_readable(tx->state));
  if (tx->find_file(path) == nullptr) {
    return fail(ErrCode::notfound, "no such file: " + path.str());
  }
  return store_.open_read(id, path);
}

// --- Peer -------------------------------------------------------------------

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
      .state = State::open,
      .created_at = req.created_at,
      .expires_at = req.expires_at,
  };
  CFD_TRYV(repo_.insert(tx));  // `conflict` if the id exists
  return tx;
}

// --- Forwarder and retention ------------------------------------------------

Result<void> TransactionService::mark_delivered(const common::TransactionId& id) {
  const auto mu = lock_for(id);
  const std::scoped_lock lock(*mu);

  auto tx = repo_.find(id);
  if (!tx) {
    return not_found(id);
  }
  CFD_TRYV(check_transition(tx->state, State::delivered));
  tx->state = State::delivered;
  tx->delivered_at = clock_.now();
  CFD_TRYV(repo_.save(*tx));

  auto ev = event_for(BizEventType::forwarded, *tx, Caller{.name = "system", .is_peer = false});
  ev.file_count = static_cast<std::uint32_t>(tx->files.size());
  ev.total_bytes = tx->total_bytes();
  blog_.record(ev);
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
  {
    const auto mu = lock_for(id);
    const std::scoped_lock lock(*mu);

    auto tx = repo_.find(id);
    if (!tx) {
      return not_found(id);
    }
    // Already `expired` (an earlier delete failed): just try the delete again.
    if (tx->state != State::expired) {
      CFD_TRYV(check_transition(tx->state, State::expired));
      tx->state = State::expired;
      CFD_TRYV(repo_.save(*tx));
    }
    CFD_TRYV(repo_.remove(id));

    blog_.record(event_for(BizEventType::expired, *tx, Caller{.name = "system", .is_peer = false}));
  }

  const std::scoped_lock lock(locks_mu_);
  locks_.erase(id);
  return {};
}

}  // namespace confide::transaction
