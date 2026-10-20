#pragma once
#include "common/error.hpp"
#include "common/id.hpp"
#include "logging/tech_log.hpp"
#include "net/http_client.hpp"
#include "storage/storage.hpp"
#include "transaction/service.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace confide::forwarder {

using common::Result;

// A server we push to, from a [[peer]] block.
struct Peer {
  std::string name;
  net::ServerUrl url;
  std::string token;  // the token we present to it (PeerCfg::token)
};

struct ForwarderOptions {
  std::vector<Peer> peers;
  std::optional<std::filesystem::path> ca_file{};  // verifies https peers; system CAs if unset
  // Longest a single connect, read or write may stall. Generous, because
  // the peer re-hashes every file before it answers the commit.
  std::chrono::milliseconds io_timeout{std::chrono::minutes{5}};
  // Wait before the first retry; doubles per failed attempt up to max_retry.
  std::chrono::milliseconds first_retry{std::chrono::seconds{5}};
  std::chrono::milliseconds max_retry{std::chrono::minutes{10}};
};

// FR-10, one-hop forwarding (Architecture §7): pushes each committed
// transaction for a user on another server to that server, then marks it
// `delivered`. A push is three steps on the peer's /internal API:
//
//   POST /internal/transactions                      metadata, same id
//   PUT  /internal/transactions/{id}/files/{path*}   each file, streamed
//   POST /internal/transactions/{id}/commit
//
// A failed push leaves the transaction `committed` and is retried with
// exponential backoff while the error may go away by itself (network,
// peer overloaded or failing: `unavailable`, `throttled`, `internal`,
// `io`, `integrity`). Any other error (unknown user on the peer, wrong
// peer token, ...) is logged and the transaction waits for the next start,
// since fixing it means changing a config. A retry first asks the peer how
// far the earlier push got, so files are sent again only into a
// transaction that is still open there.
//
// One worker thread pushes one transaction at a time. Thread-safe.
class Forwarder {
public:
  Forwarder(transaction::TransactionService& service, const storage::FileStore& store,
            logging::TechLog& tlog, ForwarderOptions options);
  ~Forwarder();  // stop()

  Forwarder(const Forwarder&) = delete;
  Forwarder& operator=(const Forwarder&) = delete;
  Forwarder(Forwarder&&) = delete;
  Forwarder& operator=(Forwarder&&) = delete;

  // Queues a transaction for pushing; ignored if it is queued already.
  // Wired to TransactionService's CommitHook for remote targets.
  void enqueue(const common::TransactionId& id);

  // Queues every transaction still waiting from before
  // (TransactionService::list_outgoing()) and starts the worker thread.
  // Does nothing if it is already running.
  void start();

  // Aborts a push in progress, which is retried on the next start, and
  // waits for the worker. Safe to call more than once.
  void stop();

  // One push attempt on the calling thread. Used by the worker and by tests.
  Result<void> push(const common::TransactionId& id);

  // Whether an error from push() may go away on a later attempt.
  static bool is_transient(common::ErrCode code);

  // Transactions queued and not being pushed right now.
  std::size_t queued() const;

private:
  struct Pending {
    common::TransactionId id;
    std::uint32_t attempts = 0;
    std::chrono::steady_clock::time_point due;
  };

  void run();
  std::chrono::milliseconds backoff(std::uint32_t attempts) const;
  const Peer* find_peer(std::string_view name) const;

  transaction::TransactionService& service_;
  const storage::FileStore& store_;
  logging::TechLog& tlog_;
  ForwarderOptions options_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Pending> queue_;
  bool stop_ = false;
  net::HttpClient* active_ = nullptr;  // client of the push in progress, for stop()
  std::thread worker_;
};

}  // namespace confide::forwarder
