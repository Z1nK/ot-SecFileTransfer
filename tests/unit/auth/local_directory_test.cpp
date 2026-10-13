#include <auth/local_directory.hpp>
#include <auth/password.hpp>
#include <auth/user_directory.hpp>
#include <config/config.hpp>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

using confide::auth::LocalDirectory;
using confide::auth::make_directory;
using confide::auth::PasswordHash;
using confide::common::ErrCode;
using confide::config::AuthCfg;
using confide::config::Config;
using confide::config::GroupCfg;
using confide::config::LogCfg;
using confide::config::RestCfg;
using confide::config::RetentionCfg;
using confide::config::StorageCfg;
using confide::config::TlsCfg;
using confide::config::UserCfg;

namespace {

std::string hash_of(std::string_view password) {
  return PasswordHash::create(password, 1000).value().to_string();
}

Config make_config(AuthCfg auth) {
  return Config("siteA", StorageCfg{.root = "/tmp/x"}, RestCfg{},
                TlsCfg{.enabled = false, .cert_path = {}, .key_path = {}, .ca_path = {}},
                std::move(auth), LogCfg{}, RetentionCfg{});
}

class LocalDirectoryTest : public ::testing::Test {
protected:
  LocalDirectoryTest()
      : directory_(
            LocalDirectory::create(
                make_config(AuthCfg{
                    .users = {{.name = "alice", .password_hash = hash_of("alice-pw")},
                              {.name = "bob", .password_hash = hash_of("bob-pw")}},
                    .groups = {GroupCfg{.name = "ops", .members = {"alice"}}},
                }))
                .value()) {}

  std::unique_ptr<LocalDirectory> directory_;
};

}  // namespace

TEST_F(LocalDirectoryTest, VerifiesPasswords) {
  EXPECT_EQ(directory_->verify("alice", "alice-pw").value(), "alice");
  EXPECT_EQ(directory_->verify("alice", "alice-pw").value(), "alice");  // cached
  auto wrong = directory_->verify("alice", "bob-pw");
  auto unknown = directory_->verify("carol", "alice-pw");
  ASSERT_FALSE(wrong.has_value());
  ASSERT_FALSE(unknown.has_value());
  EXPECT_EQ(wrong.error().code, ErrCode::auth);
  EXPECT_EQ(unknown.error().code, ErrCode::auth);
  EXPECT_EQ(wrong.error().detail, unknown.error().detail);
}

TEST_F(LocalDirectoryTest, KnowsUsersAndGroups) {
  EXPECT_TRUE(directory_->has_user("alice").value());
  EXPECT_FALSE(directory_->has_user("ops").value());
  EXPECT_FALSE(directory_->has_user("carol").value());
  EXPECT_TRUE(directory_->has_group("ops").value());
  EXPECT_FALSE(directory_->has_group("alice").value());
}

TEST_F(LocalDirectoryTest, Membership) {
  EXPECT_TRUE(directory_->is_member("alice", "ops"));
  EXPECT_FALSE(directory_->is_member("bob", "ops"));
  EXPECT_FALSE(directory_->is_member("alice", "nope"));
}

TEST(MakeDirectory, LocalIsTheDefault) {
  auto directory = make_directory(
      make_config(AuthCfg{.users = {{.name = "alice", .password_hash = hash_of("pw")}}}));
  ASSERT_TRUE(directory.has_value()) << directory.error().detail;
  EXPECT_TRUE((*directory)->has_user("alice").value());
}

TEST(MakeDirectory, BadLocalUserFails) {
  auto directory =
      make_directory(make_config(AuthCfg{.users = {{.name = "alice", .password_hash = "x"}}}));
  ASSERT_FALSE(directory.has_value());
  EXPECT_EQ(directory.error().code, ErrCode::validation);
}
