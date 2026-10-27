#include <gtest/gtest.h>
#include <logging/business_log.hpp>
#include <logging/business_log_reader.hpp>

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
using confide::logging::BizLogQuery;
using confide::logging::BusinessLog;
using confide::logging::FileSink;
using confide::logging::ILogSink;
using confide::logging::LogLevel;
using confide::logging::read_business_log;

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
  const fs::path dir = fs::temp_directory_path()
      / (std::string{"confide_business_log_test_"}
         + ::testing::UnitTest::GetInstance()->current_test_info()->name());
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

TEST(BusinessLog, AdminEventsWithoutTransaction) {
  const auto line = BusinessLog::format({.type = BizEventType::sessions_revoked,
                                         .tx = std::nullopt,
                                         .actor = "root",
                                         .subject = "bob"},
                                        kT);
  EXPECT_EQ(line, R"({"v":1,"ts":"2026-09-27T12:00:03Z","event":"sessions_revoked",)"
                  R"("actor":"root","user":"bob"})");
}

TEST(BusinessLog, RetentionChangeCarriesDays) {
  const auto line = BusinessLog::format({.type = BizEventType::retention_changed,
                                         .tx = tx_id(),
                                         .actor = "root",
                                         .retention_days = 90},
                                        kT);
  EXPECT_EQ(line, R"({"v":1,"ts":"2026-09-27T12:00:03Z","event":"retention_changed",)"
                  R"("tx":"3f2a1b4c-5d6e-4f70-8a9b-0c1d2e3f4a5b","actor":"root","days":90})");
}

namespace {

// A business.log (and business.log.1) in a fresh temp folder.
struct LogDir {
  fs::path dir = fs::temp_directory_path()
                 / ("confide-bizlog-"
                    + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::path log = dir / "business.log";

  LogDir() { fs::create_directories(dir); }
  ~LogDir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  LogDir(const LogDir&) = delete;
  LogDir& operator=(const LogDir&) = delete;
  LogDir(LogDir&&) = delete;
  LogDir& operator=(LogDir&&) = delete;

  // One line per hour offset from kT, with the offset as actor.
  static void write(const fs::path& path, std::initializer_list<int> hours) {
    std::ofstream out(path, std::ios::binary);
    for (const int h : hours) {
      const BizEvent e{.type = BizEventType::tx_created, .tx = tx_id(), .actor = std::to_string(h)};
      out << BusinessLog::format(e, kT + std::chrono::hours{h}) << '\n';
    }
  }
};

std::vector<std::string> actors(const std::vector<std::string>& lines) {
  std::vector<std::string> out;
  for (const auto& l : lines) {
    const auto at = l.find(R"("actor":")") + 9;
    out.push_back(l.substr(at, l.find('"', at) - at));
  }
  return out;
}

}  // namespace

TEST(BusinessLogReader, ReadsRotatedFileFirstAndFiltersByTime) {
  LogDir d;
  LogDir::write(fs::path{d.log} += ".1", {0, 1, 2});
  LogDir::write(d.log, {3, 4, 5});
  const auto lines =
      read_business_log(d.log, {.from = kT + 1h, .to = kT + 5h, .limit = 100}).value();
  EXPECT_EQ(actors(lines), (std::vector<std::string>{"1", "2", "3", "4"}));
}

TEST(BusinessLogReader, StopsAtLimit) {
  LogDir d;
  LogDir::write(fs::path{d.log} += ".1", {0, 1});
  LogDir::write(d.log, {2, 3});
  const auto lines = read_business_log(d.log, {.from = kT, .to = kT + 10h, .limit = 3}).value();
  EXPECT_EQ(actors(lines), (std::vector<std::string>{"0", "1", "2"}));
}

TEST(BusinessLogReader, SkipsBadLinesAndMissingFiles) {
  LogDir d;
  {
    std::ofstream out(d.log, std::ios::binary);
    out << "not json\n" << R"({"ts":"garbage"})" << '\n';
  }
  LogDir::write(fs::path{d.log} += ".1", {1});
  const auto lines = read_business_log(d.log, {.from = kT, .to = kT + 10h}).value();
  EXPECT_EQ(actors(lines), (std::vector<std::string>{"1"}));

  EXPECT_TRUE(read_business_log(d.dir / "nothing.log", {.from = kT, .to = kT + 1h})->empty());
}
