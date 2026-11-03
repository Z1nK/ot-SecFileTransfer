#include "retention/scheduler.hpp"

#include <string_view>

namespace confide::retention {

namespace {
constexpr std::string_view kTag = "retention";
}  // namespace

RetentionScheduler::RetentionScheduler(transaction::TransactionService& service,
                                       logging::TechLog& tlog, std::chrono::seconds interval)
    : service_(service), tlog_(tlog), interval_(interval) {}

RetentionScheduler::~RetentionScheduler() { stop(); }

void RetentionScheduler::start() {
  if (worker_.joinable()) {
    return;
  }
  {
    const std::scoped_lock lock(mu_);
    stop_ = false;
  }
  worker_ = std::thread([this] { run(); });
}

void RetentionScheduler::stop() {
  {
    const std::scoped_lock lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

SweepResult RetentionScheduler::run_once() {
  SweepResult result;
  for (const auto& id : service_.list_expired()) {
    if (auto res = service_.expire(id); res) {
      ++result.expired;
    } else {
      ++result.failed;
      tlog_.warn(kTag, "expire {} failed: [{}] {}", id.str(), common::to_string(res.error().code),
                 res.error().detail);
    }
  }
  if (result.expired != 0 || result.failed != 0) {
    tlog_.info(kTag, "sweep: {} expired, {} failed", result.expired, result.failed);
  }
  return result;
}

void RetentionScheduler::run() {
  tlog_.info(kTag, "started, sweep every {}s", interval_.count());
  std::unique_lock lock(mu_);
  while (!stop_) {
    lock.unlock();
    run_once();
    lock.lock();
    cv_.wait_for(lock, interval_, [this] { return stop_; });
  }
  tlog_.info(kTag, "stopped");
}

}  // namespace confide::retention
