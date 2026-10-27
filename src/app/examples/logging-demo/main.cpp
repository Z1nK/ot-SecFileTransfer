// Usage example for the logging module: TechLog (async, levelled, may drop
// under load) and BusinessLog (sync audit trail, never drops), side by side.
// Both write one JSON object per line.
//
//   example-logging-demo              # business log goes to a file under the system temp dir
//   example-logging-demo path/to/business.log

#include <logging/business_log.hpp>
#include <logging/tech_log.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>

using confide::common::SystemClock;
using confide::common::TransactionId;
using confide::logging::BizEvent;
using confide::logging::BizEventType;
using confide::logging::BusinessLog;
using confide::logging::ConsoleSink;
using confide::logging::FileSink;
using confide::logging::LogLevel;
using confide::logging::TechLog;

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  const std::span args(argv, static_cast<std::size_t>(argc));
  const fs::path biz_path = args.size() > 1 ? fs::path(args[1])
                                            : fs::temp_directory_path() / "confide-business.log";
  fs::remove(biz_path);

  const SystemClock clock;
  {
    // Technical log: for operators. Records go through a queue to a writer
    // thread, so log() never blocks on I/O; the destructor drains the queue.
    TechLog tlog(std::make_unique<ConsoleSink>());
    tlog.set_level(LogLevel::debug);

    // Business log: for audit. FileSink rotates to <path>.1 past rotate_bytes
    // (0 = never) and does not create the parent directory.
    BusinessLog blog(std::make_unique<FileSink>(biz_path, 0), clock);

    tlog.info("demo", "starting, business log at {}", biz_path.string());
    tlog.trace("demo", "below the level, filtered out before formatting");

    // A transaction's life: technical details go to TechLog, the facts of
    // who sent what to whom go to BusinessLog.
    const auto id = TransactionId::generate();
    blog.record({.type = BizEventType::tx_created,
                 .tx = id,
                 .actor = "alice",
                 .from_user = "alice",
                 .to_user = "bob",
                 .to_server = "siteB"});
    tlog.debug("api", "POST /transactions -> 201 tx={}", id.str());

    blog.record({.type = BizEventType::file_added,
                 .tx = id,
                 .actor = "alice",
                 .file = "report/a.csv",
                 .size = 10240,
                 .sha256 = std::string(64, 'a')});
    tlog.warn("storage", "slow fsync: {} ms", 250);

    blog.record({.type = BizEventType::committed,
                 .tx = id,
                 .actor = "alice",
                 .file_count = 1,
                 .total_bytes = 10240});

    tlog.error("forwarder", "push to {} failed: {}, will retry", "siteB", "connection refused");
    blog.record({.type = BizEventType::forwarded, .tx = id, .actor = "system", .to_server = "siteB"});

    tlog.info("demo", "done, dropped={}", tlog.dropped());
  }  // TechLog flushed and its writer thread joined here

  std::cout << "\n== " << biz_path.string() << " ==\n";
  std::ifstream in(biz_path);
  for (std::string line; std::getline(in, line);) {
    std::cout << line << '\n';
  }
  return 0;
}
