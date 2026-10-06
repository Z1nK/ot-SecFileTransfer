#include <logging/tech_log.hpp>

#include <gtest/gtest.h>

#include <format>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using confide::logging::ILogSink;
using confide::logging::LogLevel;
using confide::logging::TechLog;

namespace {

struct Captured {
  LogLevel level;
  std::string line;
};

// The sink is owned by TechLog and written from its writer thread, so the
// records live in a shared, locked store the test keeps a handle to.
struct Store {
  std::mutex mu;
  std::vector<Captured> records;

  std::vector<Captured> snapshot() {
    std::lock_guard lock(mu);
    return records;
  }
};

class CaptureSink final : public ILogSink {
public:
  explicit CaptureSink(std::shared_ptr<Store> store) : store_(std::move(store)) {}

  void write(LogLevel level, std::string_view jsonl) override {
    std::lock_guard lock(store_->mu);
    store_->records.push_back({level, std::string{jsonl}});
  }

private:
  std::shared_ptr<Store> store_;
};

// Blocks inside the first write() until released, so the test can fill the
// queue while the writer thread is known to be busy.
class BlockingSink final : public ILogSink {
public:
  BlockingSink(std::shared_ptr<Store> store, std::promise<void>& entered,
               std::shared_future<void> release)
      : store_(std::move(store)), entered_(entered), release_(std::move(release)) {}

  void write(LogLevel level, std::string_view jsonl) override {
    if (first_) {
      first_ = false;
      entered_.set_value();
      release_.wait();
    }
    std::lock_guard lock(store_->mu);
    store_->records.push_back({level, std::string{jsonl}});
  }

private:
  std::shared_ptr<Store> store_;
  std::promise<void>& entered_;
  std::shared_future<void> release_;
  bool first_ = true;
};

// Destroying TechLog drains the queue and joins the writer, so afterwards
// every accepted record has reached the sink.
std::vector<Captured> run_and_collect(void (*body)(TechLog&)) {
  auto store = std::make_shared<Store>();
  {
    TechLog log(std::make_unique<CaptureSink>(store));
    body(log);
  }
  return store->snapshot();
}

}  // namespace

TEST(LogLevelToString, AllLevels) {
  EXPECT_EQ(to_string(LogLevel::trace), "trace");
  EXPECT_EQ(to_string(LogLevel::debug), "debug");
  EXPECT_EQ(to_string(LogLevel::info), "info");
  EXPECT_EQ(to_string(LogLevel::warn), "warn");
  EXPECT_EQ(to_string(LogLevel::error), "error");
}

TEST(TechLog, DefaultLevelIsInfo) {
  TechLog log(std::make_unique<CaptureSink>(std::make_shared<Store>()));
  EXPECT_EQ(log.level(), LogLevel::info);
}

TEST(TechLog, WritesJsonLineWithFields) {
  auto recs = run_and_collect([](TechLog& log) { log.info("api", "listening on {}", 8080); });

  ASSERT_EQ(recs.size(), 1U);
  EXPECT_EQ(recs[0].level, LogLevel::info);
  const std::string& line = recs[0].line;
  EXPECT_TRUE(line.starts_with(R"({"ts":)")) << line;
  EXPECT_TRUE(line.ends_with("}")) << line;
  EXPECT_NE(line.find(R"("lvl":"info")"), std::string::npos) << line;
  EXPECT_NE(line.find(R"("tag":"api")"), std::string::npos) << line;
  EXPECT_NE(line.find(R"("msg":"listening on 8080")"), std::string::npos) << line;
  EXPECT_NE(line.find(R"("tid":)"), std::string::npos) << line;
}

TEST(TechLog, LevelHelpersUseMatchingLevel) {
  auto recs = run_and_collect([](TechLog& log) {
    log.set_level(LogLevel::trace);
    log.trace("t", "a");
    log.debug("t", "b");
    log.info("t", "c");
    log.warn("t", "d");
    log.error("t", "e");
  });

  ASSERT_EQ(recs.size(), 5U);
  const LogLevel expected[] = {LogLevel::trace, LogLevel::debug, LogLevel::info, LogLevel::warn,
                               LogLevel::error};
  for (size_t i = 0; i < recs.size(); ++i) {
    EXPECT_EQ(recs[i].level, expected[i]);
    EXPECT_NE(recs[i].line.find(std::format(R"("lvl":"{}")", to_string(expected[i]))),
              std::string::npos);
  }
}

TEST(TechLog, DropsRecordsBelowLevel) {
  auto recs = run_and_collect([](TechLog& log) {
    log.set_level(LogLevel::warn);
    log.debug("t", "hidden");
    log.info("t", "hidden");
    log.warn("t", "shown");
    log.error("t", "shown");
  });

  ASSERT_EQ(recs.size(), 2U);
  EXPECT_EQ(recs[0].level, LogLevel::warn);
  EXPECT_EQ(recs[1].level, LogLevel::error);
}

TEST(TechLog, PreservesOrderFromOneThread) {
  auto recs = run_and_collect([](TechLog& log) {
    for (int i = 0; i < 100; ++i) {
      log.info("seq", "{}", i);
    }
  });

  ASSERT_EQ(recs.size(), 100U);
  for (int i = 0; i < 100; ++i) {
    EXPECT_NE(recs[i].line.find(std::format(R"("msg":"{}")", i)), std::string::npos);
  }
}

TEST(TechLog, EscapesJsonSpecialCharacters) {
  auto recs = run_and_collect([](TechLog& log) {
    log.info("t\"ag", "{}", std::string_view{"q\" b\\ n\n r\r t\t c\x01"});
  });

  ASSERT_EQ(recs.size(), 1U);
  const std::string& line = recs[0].line;
  EXPECT_NE(line.find(R"("tag":"t\"ag")"), std::string::npos) << line;
  EXPECT_NE(line.find(R"("msg":"q\" b\\ n\n r\r t\t c\u0001")"), std::string::npos) << line;
  EXPECT_EQ(line.find('\n'), std::string::npos) << "raw newline breaks JSON lines";
}

TEST(TechLog, CountsDroppedWhenQueueFull) {
  auto store = std::make_shared<Store>();
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> release_f = release.get_future().share();
  {
    TechLog log(std::make_unique<BlockingSink>(store, entered, release_f), /*capacity=*/1);

    log.info("t", "first");  // taken by the writer, which then blocks in the sink
    entered.get_future().wait();

    log.info("t", "queued");   // fills the capacity-1 queue
    log.info("t", "dropped");  // no room
    log.info("t", "dropped");
    EXPECT_EQ(log.dropped(), 2U);

    release.set_value();
  }

  auto recs = store->snapshot();
  ASSERT_EQ(recs.size(), 2U);
  EXPECT_NE(recs[0].line.find(R"("msg":"first")"), std::string::npos);
  EXPECT_NE(recs[1].line.find(R"("msg":"queued")"), std::string::npos);
}

TEST(TechLog, DestructorFlushesPendingRecords) {
  auto recs = run_and_collect([](TechLog& log) {
    for (int i = 0; i < 1000; ++i) {
      log.info("t", "{}", i);
    }
  });
  EXPECT_EQ(recs.size(), 1000U);
}

TEST(TechLog, ConcurrentProducersLoseNothing) {
  constexpr int kThreads = 4;
  constexpr int kPerThread = 500;
  auto store = std::make_shared<Store>();
  {
    TechLog log(std::make_unique<CaptureSink>(store));
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&log, t] {
        for (int i = 0; i < kPerThread; ++i) {
          log.info("mt", "{}-{}", t, i);
        }
      });
    }
    for (auto& th : threads) {
      th.join();
    }
    EXPECT_EQ(log.dropped(), 0U);
  }
  EXPECT_EQ(store->snapshot().size(), static_cast<size_t>(kThreads * kPerThread));
}
