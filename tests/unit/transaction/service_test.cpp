#include <gtest/gtest.h>
#include <transaction/service.hpp>

#include <cctype>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

  void start(MembershipCheck is_recipient, confide::transaction::DestinationCheck check = {}) {
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
                                                            .check_destination = std::move(check),
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

// The membership check can be a slow LDAP call. It must run once per target
// name and without the repository lock, or one inbox listing stalls every
// other request for as long as the directory takes to answer.
TEST_F(GroupTargetTest, InboxAsksAboutMembershipOutsideTheRepositoryLock) {
  int calls = 0;
  bool repo_was_locked = false;
  std::optional<confide::common::TransactionId> known;
  // Kept until the end of the test: a future from std::async joins its
  // thread when destroyed, which inside the callback would wait for the very
  // lock being tested and hang instead of failing.
  std::vector<std::future<bool>> probes;
  start([&](std::string_view user, std::string_view target) {
    ++calls;
    // Another thread must be able to read the repository meanwhile.
    auto& probe = probes.emplace_back(
        std::async(std::launch::async, [&] { return repo_->find(*known).has_value(); }));
    if (probe.wait_for(std::chrono::seconds{2}) != std::future_status::ready) {
      repo_was_locked = true;
    }
    return in_devs(user, target);
  });

  for (int i = 0; i < 3; ++i) {
    const auto tx = create_open();
    known = tx.id;
    ASSERT_TRUE(service_->commit(carol_, tx.id).has_value());
  }

  calls = 0;
  EXPECT_EQ(service_->list(alice_, Box::in).size(), 3U);
  EXPECT_EQ(calls, 1);  // three transactions, one target name
  EXPECT_FALSE(repo_was_locked);

  calls = 0;
  EXPECT_TRUE(service_->list(dave_, Box::in).empty());
  EXPECT_EQ(calls, 1);
  EXPECT_FALSE(repo_was_locked);
}

// Each transaction has a mutex, kept in a map. Asking about ids that do not
// exist must not add to it, or any user could grow it without end.
TEST_F(GroupTargetTest, UnknownIdsLeaveNoLockBehind) {
  const auto tx = create_open();
  const auto before = service_->lock_count();
  EXPECT_EQ(before, 1U);

  const Caller admin{.name = "root", .is_admin = true};
  for (int i = 0; i < 50; ++i) {
    const auto ghost = confide::common::TransactionId::generate();
    EXPECT_EQ(service_->commit(carol_, ghost).error().code, ErrCode::notfound);
    EXPECT_EQ(service_->begin_upload(carol_, ghost, path_).error().code, ErrCode::notfound);
    EXPECT_EQ(service_->remove_file(carol_, ghost, path_).error().code, ErrCode::notfound);
    EXPECT_EQ(service_->set_retention(admin, ghost, 1).error().code, ErrCode::notfound);
    EXPECT_EQ(service_->admin_delete(admin, ghost).error().code, ErrCode::notfound);
    EXPECT_EQ(service_->mark_delivered(ghost).error().code, ErrCode::notfound);
  }
  EXPECT_EQ(service_->lock_count(), before);

  // The real one still works, and its mutex goes when it is deleted.
  ASSERT_TRUE(service_->commit(carol_, tx.id).has_value());
  ASSERT_TRUE(service_->admin_delete(admin, tx.id).has_value());
  EXPECT_EQ(service_->lock_count(), 0U);
}

namespace {

// A directory that ignores case, as LDAP does: "Alice" is the user "alice".
confide::common::Result<std::string> loose_directory(std::string_view user, std::string_view) {
  std::string lower{user};
  for (char& c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (lower != "alice") {
    return std::unexpected(
        confide::common::Error::make(ErrCode::unknown_destination, "no such user"));
  }
  return lower;
}

}  // namespace

// The target is stored as the directory spells it, not as the sender typed
// it: alice logs in as "alice" and must find what was sent to "Alice".
TEST_F(GroupTargetTest, TargetIsStoredAsTheDirectorySpellsIt) {
  start(in_devs, loose_directory);
  auto tx = service_->create(carol_, CreateRequest{.target_user = "Alice", .target_server = {}});
  ASSERT_TRUE(tx.has_value());
  EXPECT_EQ(tx->target_user, "alice");
  ASSERT_TRUE(service_->commit(carol_, tx->id).has_value());

  const auto inbox = service_->list(alice_, Box::in);
  ASSERT_EQ(inbox.size(), 1U);
  EXPECT_EQ(inbox[0].id, tx->id);
  EXPECT_TRUE(service_->get(alice_, tx->id).has_value());
}

// What a peer pushes is checked like our own input.
TEST_F(GroupTargetTest, PeerPushIsValidated) {
  start(in_devs, loose_directory);
  const Caller peer{.name = "siteB", .is_peer = true};
  const auto now = clock_.now();
  const auto push = [&](std::string sender, std::string target, std::chrono::days keep) {
    return service_->receive(
        peer, confide::transaction::ReceiveRequest{.id = confide::common::TransactionId::generate(),
                                                   .sender = std::move(sender),
                                                   .target_user = std::move(target),
                                                   .created_at = now,
                                                   .expires_at = now + keep});
  };

  // Sender names follow the rule for user names: nothing that could break a
  // one-line listing or a terminal.
  for (const char* bad : {"eve mallory", "eve\x1b[2J", "eve@siteC", "a/b", ""}) {
    auto r = push(bad, "alice", std::chrono::days{1});
    ASSERT_FALSE(r.has_value()) << bad;
    EXPECT_EQ(r.error().code, ErrCode::validation) << bad;
  }

  // The target gets the local spelling here as well.
  auto ok = push("eve", "ALICE", std::chrono::days{1});
  ASSERT_TRUE(ok.has_value()) << ok.error().detail;
  EXPECT_EQ(ok->target_user, "alice");
  EXPECT_EQ(ok->expires_at, now + std::chrono::days{1});

  // max_retention_days is 7 in this fixture: a longer stay is cut to it.
  auto forever = push("eve", "alice", std::chrono::days{3650});
  ASSERT_TRUE(forever.has_value()) << forever.error().detail;
  EXPECT_EQ(forever->expires_at, now + std::chrono::days{7});
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

// --- Admin ------------------------------------------------------------------

namespace {

using AdminServiceTest = GroupTargetTest;
using confide::transaction::AdminFilter;
using confide::transaction::State;

const Caller kAdmin{.name = "root", .is_admin = true};

}  // namespace

TEST_F(AdminServiceTest, NonAdminsGetNothing) {
  const auto tx = create_open();
  EXPECT_TRUE(service_->admin_list(carol_, {}).empty());
  EXPECT_EQ(service_->admin_get(carol_, tx.id).error().code, ErrCode::forbidden);
  EXPECT_EQ(service_->admin_delete(carol_, tx.id).error().code, ErrCode::forbidden);
  EXPECT_EQ(service_->set_retention(carol_, tx.id, 3).error().code, ErrCode::forbidden);
}

TEST_F(AdminServiceTest, ListsEveryTransactionWithFilters) {
  const auto open = create_open();
  const auto done = create_open();
  ASSERT_TRUE(service_->commit(carol_, done.id).has_value());

  EXPECT_EQ(service_->admin_list(kAdmin, {}).size(), 2U);
  const auto committed = service_->admin_list(kAdmin, AdminFilter{.state = State::committed});
  ASSERT_EQ(committed.size(), 1U);
  EXPECT_EQ(committed[0].id, done.id);
  EXPECT_EQ(service_->admin_list(kAdmin, AdminFilter{.user = "devs"}).size(), 2U);
  EXPECT_EQ(service_->admin_list(kAdmin, AdminFilter{.user = "carol"}).size(), 2U);
  EXPECT_TRUE(service_->admin_list(kAdmin, AdminFilter{.user = "dave"}).empty());
  const auto now = clock_.now();
  EXPECT_EQ(service_->admin_list(kAdmin, AdminFilter{.from = now}).size(), 2U);
  EXPECT_TRUE(service_->admin_list(kAdmin, AdminFilter{.to = now}).empty());

  // Open transactions too, with their files: admins see all metadata.
  const auto got = service_->admin_get(kAdmin, open.id).value();
  EXPECT_EQ(got.state, State::open);
  EXPECT_EQ(got.files.size(), 1U);
}

TEST_F(AdminServiceTest, DeletesInAnyState) {
  const auto open = create_open();
  const auto done = create_open();
  ASSERT_TRUE(service_->commit(carol_, done.id).has_value());

  ASSERT_TRUE(service_->admin_delete(kAdmin, open.id).has_value());
  ASSERT_TRUE(service_->admin_delete(kAdmin, done.id).has_value());
  EXPECT_TRUE(service_->admin_list(kAdmin, {}).empty());
  EXPECT_EQ(service_->get(carol_, done.id).error().code, ErrCode::notfound);
  EXPECT_EQ(service_->admin_delete(kAdmin, done.id).error().code, ErrCode::notfound);
}

TEST_F(AdminServiceTest, SetsRetentionFromCreation) {
  const auto tx = create_open();
  const auto changed = service_->set_retention(kAdmin, tx.id, 5).value();
  EXPECT_EQ(changed.expires_at, tx.created_at + std::chrono::days{5});
  EXPECT_EQ(service_->admin_get(kAdmin, tx.id)->expires_at, changed.expires_at);

  // 1..max_retention_days (7 here).
  EXPECT_EQ(service_->set_retention(kAdmin, tx.id, 0).error().code, ErrCode::validation);
  EXPECT_EQ(service_->set_retention(kAdmin, tx.id, 8).error().code, ErrCode::validation);
}
