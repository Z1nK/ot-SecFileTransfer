#include <client/recent_recipients.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace confide::client {
namespace {

namespace fs = std::filesystem;
using List = std::vector<std::string>;

TEST(RecentRecipients, NewOneGoesFirst) {
  EXPECT_EQ(detail::with_recent({"bob", "carol"}, "alice"), (List{"alice", "bob", "carol"}));
}

TEST(RecentRecipients, KnownOneMovesToFront) {
  EXPECT_EQ(detail::with_recent({"bob", "carol", "alice"}, "alice"),
            (List{"alice", "bob", "carol"}));
}

TEST(RecentRecipients, KeepsAtMostMax) {
  EXPECT_EQ(detail::with_recent({"a", "b", "c"}, "d", 3), (List{"d", "a", "b"}));
}

TEST(RecentRecipients, IgnoresEmptyAndMultiLine) {
  EXPECT_EQ(detail::with_recent({"a"}, ""), (List{"a"}));
  EXPECT_EQ(detail::with_recent({"a"}, "x\ny"), (List{"a"}));
  EXPECT_EQ(detail::with_recent({"a"}, "x\r"), (List{"a"}));
}

#ifndef _WIN32
// File round trip under a throwaway XDG_STATE_HOME.
class RecentRecipientsFile : public ::testing::Test {
protected:
  void SetUp() override {
    root_ = fs::temp_directory_path()
            / std::format("ftc-recent-test-{}", ::testing::UnitTest::GetInstance()->random_seed());
    fs::remove_all(root_);
    fs::create_directories(root_);
    ::setenv("XDG_STATE_HOME", root_.c_str(), 1);  // NOLINT(concurrency-mt-unsafe)
    auto url = net::ServerUrl::parse("https://ftc.example.org:8443");
    ASSERT_TRUE(url);
    auto recent = RecentRecipients::for_server(*url, "me");
    ASSERT_TRUE(recent);
    recent_.emplace(*recent);
  }
  void TearDown() override { fs::remove_all(root_); }

  fs::path root_;
  std::optional<RecentRecipients> recent_;
};

TEST_F(RecentRecipientsFile, PathIsPerServerUnderState) {
  EXPECT_EQ(recent_->path(), root_ / "ftc" / "recent" / "me@ftc.example.org_8443");
}

TEST_F(RecentRecipientsFile, NoFileIsEmpty) {
  auto list = recent_->load();
  ASSERT_TRUE(list);
  EXPECT_TRUE(list->empty());
}

TEST_F(RecentRecipientsFile, PushNeedsTheFtcFolder) {
  EXPECT_FALSE(recent_->push("bob"));
  EXPECT_FALSE(fs::exists(root_ / "ftc"));
}

TEST_F(RecentRecipientsFile, PushAndLoad) {
  fs::create_directory(root_ / "ftc");
  for (int i = 0; i < 12; ++i) {
    ASSERT_TRUE(recent_->push(std::format("user{}", i)));
  }
  ASSERT_TRUE(recent_->push("user5"));
  auto list = recent_->load();
  ASSERT_TRUE(list);
  ASSERT_EQ(list->size(), RecentRecipients::kMax);
  EXPECT_EQ(list->front(), "user5");
  EXPECT_EQ((*list)[1], "user11");
  EXPECT_EQ(list->back(), "user2");
  EXPECT_EQ(fs::status(root_ / "ftc" / "recent").permissions() & fs::perms::all,
            fs::perms::owner_all);
}
#endif

}  // namespace
}  // namespace confide::client
