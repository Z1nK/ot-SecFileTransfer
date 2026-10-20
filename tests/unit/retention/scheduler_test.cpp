#include <gtest/gtest.h>
#include <retention/scheduler.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using confide::common::ErrCode;
using confide::common::RelativePath;
using confide::common::Timestamp;
using confide::common::TransactionId;
using confide::retention::RetentionScheduler;
using confide::transaction::Caller;
using confide::transaction::CreateRequest;
using confide::transaction::ServiceOptions;
using confide::transaction::Transaction;
using confide::transaction::TransactionRepository;
using confide::transaction::TransactionService;

namespace fs = std::filesystem;
using std::chrono::days;
using std::chrono::seconds;

namespace {

struct NullSink final : confide::logging::ILogSink {
  void write(confide::logging::LogLevel, std::string_view) override {}
};

// Keeps the business log lines, to check the `expired` events.
struct LinesSink final : confide::logging::ILogSink {
  void write(confide::logging::LogLevel, std::string_view line) override {
    const std::scoped_lock lock(mu);
    lines.emplace_back(line);
  }
  std::size_t count(std::string_view needle) {
    const std::scoped_lock lock(mu);
    std::size_t n = 0;
    for (const auto& l : lines) {
      n += l.find(needle) != std::string::npos ? 1 : 0;
    }
    return n;
  }
  std::mutex mu;
  std::vector<std::string> lines;
};

struct SettableClock final : confide::common::IClock {
  Timestamp now() const override { return Timestamp{seconds{t.load()}}; }
  void advance(seconds d) { t += d.count(); }
  std::atomic<std::int64_t> t{1'800'000'000};
};

class RetentionTest : public ::testing::Test {
protected:
  void SetUp() override {
    fs::remove_all(dir_);
    auto store = confide::storage::FileStore::open(dir_);
    ASSERT_TRUE(store.has_value()) << store.error().detail;
    store_.emplace(std::move(*store));
    repo_.emplace(*store_);
    service_ = std::make_unique<TransactionService>(*repo_, *store_, blog_, clock_,
                                                    ServiceOptions{.local_server = "siteA",
                                                                   .default_retention_days = 1,
                                                                   .max_retention_days = 7,
                                                                   .check_destination = {},
                                                                   .is_recipient = {},
                                                                   .on_committed = {}});
  }
  void TearDown() override {
    service_.reset();
    repo_.reset();
    store_.reset();
    fs::remove_all(dir_);
  }

  // alice -> bob with one file; committed unless `commit` is false.
  Transaction send(int retention_days, bool commit = true) {
    auto tx = service_->create(
        alice_,
        CreateRequest{.target_user = "bob", .target_server = {}, .retention_days = retention_days});
    EXPECT_TRUE(tx.has_value());
    auto up = service_->begin_upload(alice_, tx->id, path_);
    EXPECT_TRUE(up.has_value());
    EXPECT_TRUE(up->write(std::string_view("hello")).has_value());
    EXPECT_TRUE(up->finish().has_value());
    if (commit) {
      EXPECT_TRUE(service_->commit(alice_, tx->id).has_value());
    }
    return *tx;
  }

  bool on_disk(const TransactionId& id) const { return fs::exists(dir_ / "transactions" / id.str()); }

  fs::path dir_ = fs::temp_directory_path()
      / (std::string{"confide_retention_test_"}
         + ::testing::UnitTest::GetInstance()->current_test_info()->name());
  SettableClock clock_;
  LinesSink* biz_ = new LinesSink;  // owned by blog_
  confide::logging::BusinessLog blog_{std::unique_ptr<LinesSink>(biz_), clock_};
  confide::logging::TechLog tlog_{std::make_unique<NullSink>()};
  std::optional<confide::storage::FileStore> store_;
  std::optional<TransactionRepository> repo_;
  std::unique_ptr<TransactionService> service_;
  RelativePath path_ = RelativePath::parse("a.txt").value();
  Caller alice_{.name = "alice"};
};

}  // namespace

TEST_F(RetentionTest, ListExpiredOnlyAfterExpiry) {
  const auto tx = send(1);
  EXPECT_TRUE(service_->list_expired().empty());

  clock_.advance(days{1} - seconds{1});
  EXPECT_TRUE(service_->list_expired().empty());

  clock_.advance(seconds{1});
  const auto ids = service_->list_expired();
  ASSERT_EQ(ids.size(), 1U);
  EXPECT_EQ(ids[0], tx.id);
}

TEST_F(RetentionTest, ExpireDeletesTransactionAndFiles) {
  const auto tx = send(1);
  ASSERT_TRUE(on_disk(tx.id));
  clock_.advance(days{1});

  ASSERT_TRUE(service_->expire(tx.id).has_value());
  EXPECT_FALSE(on_disk(tx.id));
  EXPECT_EQ(service_->get(alice_, tx.id).error().code, ErrCode::notfound);
  EXPECT_TRUE(service_->list_expired().empty());
  EXPECT_EQ(biz_->count(R"("event":"expired")"), 1U);

  EXPECT_EQ(service_->expire(tx.id).error().code, ErrCode::notfound);
}

TEST_F(RetentionTest, SweepExpiresOpenAndCommittedKeepsTheRest) {
  const auto open = send(1, /*commit=*/false);
  const auto committed = send(1);
  const auto later = send(7);

  RetentionScheduler scheduler(*service_, tlog_, seconds{3600});
  EXPECT_EQ(scheduler.run_once().expired, 0U);

  clock_.advance(days{2});
  const auto result = scheduler.run_once();
  EXPECT_EQ(result.expired, 2U);
  EXPECT_EQ(result.failed, 0U);
  EXPECT_FALSE(on_disk(open.id));
  EXPECT_FALSE(on_disk(committed.id));
  EXPECT_TRUE(on_disk(later.id));
  EXPECT_TRUE(service_->get(alice_, later.id).has_value());

  EXPECT_EQ(scheduler.run_once().expired, 0U);
}

TEST_F(RetentionTest, StartSweepsAtOnceAndStopDoesNotWaitForInterval) {
  const auto tx = send(1);
  clock_.advance(days{1});

  RetentionScheduler scheduler(*service_, tlog_, seconds{3600});
  scheduler.start();
  scheduler.start();  // no second thread

  const auto deadline = std::chrono::steady_clock::now() + seconds{5};
  while (on_disk(tx.id) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  EXPECT_FALSE(on_disk(tx.id));

  const auto before = std::chrono::steady_clock::now();
  scheduler.stop();
  EXPECT_LT(std::chrono::steady_clock::now() - before, seconds{1});
  scheduler.stop();  // idempotent
}
