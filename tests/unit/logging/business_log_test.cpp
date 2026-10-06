#include <gtest/gtest.h>
#include <logging/business_log.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using confide::common::IClock;
using confide::common::Timestamp;
using confide::common::TransactionId;
using confide::logging::BizEvent;
using confide::logging::BizEventType;
using confide::logging::BusinessLog;
using confide::logging::FileSink;
using confide::logging::ILogSink;
using confide::logging::LogLevel;

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

// 2026-09-27T12:00:03Z
constexpr Timestamp kT{std::chrono::sys_days{std::chrono::year{2026} / 9 / 27} + 12h + 3s};

class FakeClock final : public IClock {
public:
  Timestamp now() const override { return kT; }
};

struct Store {
  std::mutex mu;
  std::vector<std::string> lines;
};

class CaptureSink final : public ILogSink {
public:
  explicit CaptureSink(std::shared_ptr<Store> store) : store_(std::move(store)) {}
  void write(LogLevel /*level*/, std::string_view jsonl) override {
    std::lock_guard lock(store_->mu);
    store_->lines.emplace_back(jsonl);
  }

private:
  std::shared_ptr<Store> store_;
};

TransactionId tx_id() {
  return *TransactionId::parse("3f2a1b4c-5d6e-4f70-8a9b-0c1d2e3f4a5b");
}

}  // namespace

TEST(BusinessLog, FormatsFullFileEvent) {
  const auto line = BusinessLog::format({.type = BizEventType::file_added,
                                         .tx = tx_id(),
                                         .actor = "alice",
                                         .from_user = "alice",
                                         .to_user = "bob",
                                         .to_server = "siteB",
                                         .file = "report/a.csv",
                                         .size = 10240,
                                         .sha256 = "ab12"},
                                        kT);
  EXPECT_EQ(line,
            R"({"v":1,"ts":"2026-09-27T12:00:03Z","event":"file_added",)"
            R"("tx":"3f2a1b4c-5d6e-4f70-8a9b-0c1d2e3f4a5b","actor":"alice","from":"alice",)"
            R"("to":"bob","server":"siteB","file":"report/a.csv","size":10240,"sha256":"ab12"})");
}

TEST(BusinessLog, OmitsEmptyFields) {
  const auto line = BusinessLog::format({.type = BizEventType::committed,
                                         .tx = tx_id(),
                                         .actor = "alice",
                                         .file_count = 3,
                                         .total_bytes = 300},
                                        kT);
  EXPECT_EQ(line, R"({"v":1,"ts":"2026-09-27T12:00:03Z","event":"committed",)"
                  R"("tx":"3f2a1b4c-5d6e-4f70-8a9b-0c1d2e3f4a5b","actor":"alice",)"
                  R"("files":3,"bytes":300})");
}

TEST(BusinessLog, EscapesStrings) {
  const auto line = BusinessLog::format(
      {.type = BizEventType::downloaded, .tx = tx_id(), .actor = "bob", .file = "a\"b\\c\n.txt"},
      kT);
  EXPECT_NE(line.find(R"("file":"a\"b\\c\n.txt")"), std::string::npos) << line;
}

TEST(BusinessLog, EventNames) {
  EXPECT_EQ(to_string(BizEventType::tx_created), "tx_created");
  EXPECT_EQ(to_string(BizEventType::file_removed), "file_removed");
  EXPECT_EQ(to_string(BizEventType::forwarded), "forwarded");
  EXPECT_EQ(to_string(BizEventType::received), "received");
  EXPECT_EQ(to_string(BizEventType::expired), "expired");
}

TEST(BusinessLog, RecordWritesSynchronouslyWithClockTime) {
  auto store = std::make_shared<Store>();
  FakeClock clock;
  BusinessLog log(std::make_unique<CaptureSink>(store), clock);

  log.record({.type = BizEventType::expired, .tx = tx_id(), .actor = "system"});

  // No background thread: the line is there as soon as record() returns.
  ASSERT_EQ(store->lines.size(), 1U);
  EXPECT_EQ(
      store->lines[0],
      BusinessLog::format({.type = BizEventType::expired, .tx = tx_id(), .actor = "system"}, kT));
}

TEST(BusinessLog, NeverDropsUnderConcurrency) {
  auto store = std::make_shared<Store>();
  FakeClock clock;
  BusinessLog log(std::make_unique<CaptureSink>(store), clock);

  constexpr int kThreads = 8;
  constexpr int kPerThread = 500;
  {
    std::vector<std::jthread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&] {
        for (int i = 0; i < kPerThread; ++i) {
          log.record({.type = BizEventType::file_added, .tx = tx_id(), .actor = "alice"});
        }
      });
    }
  }
  EXPECT_EQ(store->lines.size(), static_cast<size_t>(kThreads * kPerThread));
}

TEST(BusinessLog, WritesToFile) {
  const fs::path dir = fs::temp_directory_path() / "confide_business_log_test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path file = dir / "business.log";

  FakeClock clock;
  {
    BusinessLog log(std::make_unique<FileSink>(file, 0), clock);
    log.record({.type = BizEventType::tx_created, .tx = tx_id(), .actor = "alice"});
    log.record({.type = BizEventType::committed, .tx = tx_id(), .actor = "alice"});
  }

  std::ifstream in(file);
  std::string l1;
  std::string l2;
  std::getline(in, l1);
  std::getline(in, l2);
  fs::remove_all(dir);
  EXPECT_NE(l1.find(R"("event":"tx_created")"), std::string::npos);
  EXPECT_NE(l2.find(R"("event":"committed")"), std::string::npos);
}
