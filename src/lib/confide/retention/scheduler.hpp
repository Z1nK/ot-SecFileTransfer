#pragma once
#include "logging/tech_log.hpp"
#include "transaction/service.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>

namespace confide::retention {

struct SweepResult {
  std::size_t expired = 0;  // deleted with their files
  std::size_t failed = 0;   // expire() returned an error; retried next sweep
};

// FR-9: deletes transactions whose expires_at has passed (Architecture §4,
// `retention`). A worker thread sweeps once at start() and then every
// `interval`; stop() wakes it, so shutdown does not wait for the interval.
//
// The work itself is in TransactionService::list_expired() and expire();
// a failed expire() leaves the transaction `expired` and is tried again on
// the next sweep.
class RetentionScheduler {
public:
  RetentionScheduler(transaction::TransactionService& service, logging::TechLog& tlog,
                     std::chrono::seconds interval);
  ~RetentionScheduler();  // stop()

  RetentionScheduler(const RetentionScheduler&) = delete;
  RetentionScheduler& operator=(const RetentionScheduler&) = delete;
  RetentionScheduler(RetentionScheduler&&) = delete;
  RetentionScheduler& operator=(RetentionScheduler&&) = delete;

  // Starts the worker thread. Does nothing if it is already running.
  void start();

  // Stops the worker thread and waits for it, letting a running sweep finish.
  // Safe to call more than once.
  void stop();

  // One sweep on the calling thread. Used by the worker and by tests.
  SweepResult run_once();

private:
  void run();

  transaction::TransactionService& service_;
  logging::TechLog& tlog_;
  std::chrono::seconds interval_;

  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::thread worker_;
};

}  // namespace confide::retention
