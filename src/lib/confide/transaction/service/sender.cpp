#include "transaction/service/detail.hpp"
#include "transaction/state_machine.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace confide::transaction {

using namespace detail;  // NOLINT(google-build-using-namespace)

namespace {

void upsert_file(std::vector<FileEntry>& files, FileEntry entry) {
  const auto it = std::ranges::lower_bound(files, entry.path, {}, &FileEntry::path);
  if (it != files.end() && it->path == entry.path) {
    *it = std::move(entry);
  } else {
    files.insert(it, std::move(entry));
  }
}

}  // namespace

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
  if (req.retention_days > options_.max_retention_days) {
    return fail(ErrCode::validation, std::format("retention_days must be at most {}, got {}",
                                                 options_.max_retention_days, req.retention_days));
  }
  CFD_TRYV(check_comment(req.comment, options_.max_comment_length));

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
      .comment = req.comment,
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

}  // namespace confide::transaction
