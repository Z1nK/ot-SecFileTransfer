#include <gtest/gtest.h>
#include <transaction/service.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

using confide::common::ErrCode;
using confide::common::RelativePath;
using confide::transaction::Box;
using confide::transaction::Caller;
using confide::transaction::CreateRequest;
using confide::transaction::MembershipCheck;
using confide::transaction::ServiceOptions;
using confide::transaction::Transaction;
using confide::transaction::TransactionRepository;
using confide::transaction::TransactionService;

namespace fs = std::filesystem;

namespace {

struct NullSink final : confide::logging::ILogSink {
  void write(confide::logging::LogLevel, std::string_view) override {}
};

struct FixedClock final : confide::common::IClock {
  confide::common::Timestamp now() const override {
    return confide::common::Timestamp{std::chrono::seconds{1'800'000'000}};
  }
};

// devs = {alice, bob}; carol sends, dave is an outsider.
bool in_devs(std::string_view user, std::string_view target) {
  return target == "devs" && (user == "alice" || user == "bob");
}

// A service over a FileStore in a fresh temp folder; removed on scope exit.
class GroupTargetTest : public ::testing::Test {
protected:
  void SetUp() override { start(in_devs); }
  void TearDown() override {
    service_.reset();
    repo_.reset();
    store_.reset();
    fs::remove_all(dir_);
  }

  void start(MembershipCheck is_recipient) {
    fs::remove_all(dir_);
    auto store = confide::storage::FileStore::open(dir_);
    ASSERT_TRUE(store.has_value()) << store.error().detail;
    store_.emplace(std::move(*store));
    repo_.emplace(*store_);
    service_ =
        std::make_unique<TransactionService>(*repo_, *store_, blog_, clock_,
                                             ServiceOptions{.local_server = "siteA",
                                                            .default_retention_days = 1,
                                                            .max_retention_days = 7,
                                                            .check_destination = {},
                                                            .is_recipient = std::move(is_recipient),
                                                            .on_committed = {}});
  }

  // carol -> devs with one file, open.
  Transaction create_open() {
    auto tx = service_->create(carol_, CreateRequest{.target_user = "devs", .target_server = {}});
    EXPECT_TRUE(tx.has_value());
    auto up = service_->begin_upload(carol_, tx->id, path_);
    EXPECT_TRUE(up.has_value());
    EXPECT_TRUE(up->write(std::string_view("hello")).has_value());
    EXPECT_TRUE(up->finish().has_value());
    return *tx;
  }

  fs::path dir_ = fs::temp_directory_path()
      / (std::string{"confide_service_group_test_"}
         + ::testing::UnitTest::GetInstance()->current_test_info()->name());
  FixedClock clock_;
  confide::logging::BusinessLog blog_{std::make_unique<NullSink>(), clock_};
  std::optional<confide::storage::FileStore> store_;
  std::optional<TransactionRepository> repo_;
  std::unique_ptr<TransactionService> service_;
  RelativePath path_ = RelativePath::parse("a.txt").value();

  Caller carol_{.name = "carol"};
  Caller alice_{.name = "alice"};
  Caller bob_{.name = "bob"};
  Caller dave_{.name = "dave"};
};

}  // namespace

TEST_F(GroupTargetTest, MembersReadOnceCommitted) {
  const auto tx = create_open();

  // Not yet committed: invisible to members.
  EXPECT_EQ(service_->get(alice_, tx.id).error().code, ErrCode::forbidden);
  EXPECT_TRUE(service_->list(alice_, Box::in).empty());

  ASSERT_TRUE(service_->commit(carol_, tx.id).has_value());

  for (const auto& member : {alice_, bob_}) {
    const auto inbox = service_->list(member, Box::in);
    ASSERT_EQ(inbox.size(), 1U) << member.name;
    EXPECT_EQ(inbox[0].id, tx.id);
    EXPECT_TRUE(service_->get(member, tx.id).has_value()) << member.name;
    EXPECT_TRUE(service_->open_download(member, tx.id, path_).has_value()) << member.name;
  }
}

TEST_F(GroupTargetTest, OutsidersAreForbidden) {
  const auto tx = create_open();
  ASSERT_TRUE(service_->commit(carol_, tx.id).has_value());

  EXPECT_TRUE(service_->list(dave_, Box::in).empty());
  EXPECT_EQ(service_->get(dave_, tx.id).error().code, ErrCode::forbidden);
  EXPECT_EQ(service_->open_download(dave_, tx.id, path_).error().code, ErrCode::forbidden);
}

TEST_F(GroupTargetTest, MembersCannotWrite) {
  const auto tx = create_open();
  EXPECT_EQ(service_->begin_upload(alice_, tx.id, path_).error().code, ErrCode::forbidden);
  EXPECT_EQ(service_->remove_file(alice_, tx.id, path_).error().code, ErrCode::forbidden);
  EXPECT_EQ(service_->commit(alice_, tx.id).error().code, ErrCode::forbidden);
}

TEST_F(GroupTargetTest, WithoutMembershipCheckOnlyExactName) {
  start({});
  const auto tx = create_open();
  ASSERT_TRUE(service_->commit(carol_, tx.id).has_value());
  EXPECT_EQ(service_->get(alice_, tx.id).error().code, ErrCode::forbidden);
}

TEST_F(GroupTargetTest, CommentIsStoredAndLimited) {
  auto tx = service_->create(
      carol_, CreateRequest{.target_user = "devs", .target_server = {}, .comment = "Q3 reports"});
  ASSERT_TRUE(tx.has_value()) << tx.error().detail;
  EXPECT_EQ(tx->comment, "Q3 reports");
  EXPECT_EQ(service_->get(carol_, tx->id)->comment, "Q3 reports");

  EXPECT_EQ(service_
                ->create(carol_, CreateRequest{.target_user = "devs",
                                               .target_server = {},
                                               .comment = std::string(257, 'x')})
                .error()
                .code,
            ErrCode::validation);
}

TEST_F(GroupTargetTest, RetentionDefaultsAndLimit) {
  using std::chrono::days;
  const auto now = clock_.now();

  auto dflt = service_->create(carol_, CreateRequest{.target_user = "devs", .target_server = {}});
  ASSERT_TRUE(dflt.has_value()) << dflt.error().detail;
  EXPECT_EQ(dflt->expires_at, now + days{1});

  auto at_max = service_->create(carol_, CreateRequest{.target_user = "devs", .target_server = {}, .retention_days = 7});
  ASSERT_TRUE(at_max.has_value()) << at_max.error().detail;
  EXPECT_EQ(at_max->expires_at, now + days{7});

  EXPECT_EQ(service_->create(carol_, CreateRequest{.target_user = "devs", .target_server = {}, .retention_days = 8})
                .error()
                .code,
            ErrCode::validation);
}
