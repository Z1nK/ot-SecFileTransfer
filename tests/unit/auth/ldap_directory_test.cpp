#include <auth/user_directory.hpp>
#include <config/config.hpp>
#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string>

// Built only with WITH_LDAP=ON. The tests that need a directory run against
// docker/docker-compose.yml and are skipped unless CONFIDE_LDAP_TEST_URI is
// set, e.g.
//
//   docker compose -f docker/docker-compose.yml up -d
//   CONFIDE_LDAP_TEST_URI=ldap://localhost:1389 ctest -R auth

using confide::auth::IUserDirectory;
using confide::common::ErrCode;
using confide::config::AuthCfg;
using confide::config::AuthProvider;
using confide::config::Config;
using confide::config::LdapCfg;
using confide::config::LogCfg;
using confide::config::RestCfg;
using confide::config::RetentionCfg;
using confide::config::StorageCfg;
using confide::config::TlsCfg;

namespace {

// Matches docker/ldif/50-seed.ldif.
LdapCfg seed_ldap(std::string uri) {
  LdapCfg l;
  l.uri = std::move(uri);
  l.timeout_s = 2;
  l.bind_dn = "cn=admin,dc=example,dc=org";
  l.bind_password = "adminpassword";
  l.user_base_dn = "ou=people,dc=example,dc=org";
  l.group_base_dn = "ou=groups,dc=example,dc=org";
  return l;
}

std::unique_ptr<IUserDirectory> make(LdapCfg ldap) {
  AuthCfg auth;
  auth.provider = AuthProvider::ldap;
  auth.ldap = std::move(ldap);
  const Config cfg("siteA", StorageCfg{.root = "/tmp/x"}, RestCfg{},
                   TlsCfg{.enabled = false, .cert_path = {}, .key_path = {}, .ca_path = {}},
                   std::move(auth), LogCfg{}, RetentionCfg{});
  return confide::auth::make_directory(cfg).value();
}

class LdapDirectoryTest : public ::testing::Test {
protected:
  void SetUp() override {
    const char* uri = std::getenv("CONFIDE_LDAP_TEST_URI");
    if (uri == nullptr || *uri == '\0') {
      GTEST_SKIP() << "CONFIDE_LDAP_TEST_URI not set";
    }
    directory_ = make(seed_ldap(uri));
  }

  std::unique_ptr<IUserDirectory> directory_;
};

}  // namespace

TEST_F(LdapDirectoryTest, VerifiesPasswords) {
  EXPECT_EQ(directory_->verify("alice", "alice123").value(), "alice");
  EXPECT_EQ(directory_->verify("alice", "alice123").value(), "alice");  // cached
  auto wrong = directory_->verify("alice", "bob123");
  auto unknown = directory_->verify("nobody", "alice123");
  auto empty = directory_->verify("alice", "");
  ASSERT_FALSE(wrong.has_value());
  ASSERT_FALSE(unknown.has_value());
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(wrong.error().code, ErrCode::auth);
  EXPECT_EQ(unknown.error().code, ErrCode::auth);
  EXPECT_EQ(empty.error().code, ErrCode::auth);
  EXPECT_EQ(wrong.error().detail, unknown.error().detail);
}

TEST_F(LdapDirectoryTest, RejectsNamesOutsideTheCharset) {
  auto r = directory_->verify("*", "alice123");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrCode::auth);
  EXPECT_FALSE(directory_->has_user("al*").value());
}

TEST_F(LdapDirectoryTest, KnowsUsersAndGroups) {
  EXPECT_TRUE(directory_->has_user("bob").value());
  EXPECT_FALSE(directory_->has_user("nobody").value());
  EXPECT_TRUE(directory_->has_group("developers").value());
  EXPECT_FALSE(directory_->has_group("nobody").value());
}

TEST_F(LdapDirectoryTest, Membership) {
  EXPECT_TRUE(directory_->is_member("bob", "developers"));
  EXPECT_TRUE(directory_->is_member("alice", "admins"));
  EXPECT_FALSE(directory_->is_member("bob", "admins"));
  EXPECT_FALSE(directory_->is_member("carol", "developers"));
  EXPECT_FALSE(directory_->is_member("nobody", "developers"));
}

// No container needed: nothing listens on port 1.
TEST(LdapDirectory, UnreachableServerIsUnavailable) {
  auto directory = make(seed_ldap("ldap://127.0.0.1:1"));
  auto login = directory->verify("alice", "alice123");
  ASSERT_FALSE(login.has_value());
  EXPECT_EQ(login.error().code, ErrCode::unavailable);
  // Backend details stay out of what the client sees.
  EXPECT_EQ(login.error().detail.find("127.0.0.1"), std::string::npos);
  EXPECT_EQ(directory->has_user("alice").error().code, ErrCode::unavailable);
  EXPECT_FALSE(directory->is_member("alice", "admins"));
}
