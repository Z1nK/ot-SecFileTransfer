#include <auth/authenticator.hpp>
#include <auth/local_directory.hpp>
#include <auth/base64.hpp>
#include <auth/password.hpp>
#include <config/config.hpp>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

using confide::auth::Authenticator;
using confide::auth::base64_encode;
using confide::auth::IUserDirectory;
using confide::auth::LocalDirectory;
using confide::auth::PasswordHash;
using confide::common::ByteSpan;
using confide::common::ErrCode;
using confide::common::Error;
using confide::common::Result;
using confide::config::AuthCfg;
using confide::config::Config;
using confide::config::LogCfg;
using confide::config::PeerCfg;
using confide::config::RestCfg;
using confide::config::RetentionCfg;
using confide::config::StorageCfg;
using confide::config::TlsCfg;
using confide::config::UserCfg;

namespace {

std::string hash_of(std::string_view password) {
  return PasswordHash::create(password, 1000).value().to_string();
}

Config make_config(std::vector<UserCfg> users, std::vector<PeerCfg> peers = {}) {
  return Config("siteA", StorageCfg{.root = "/tmp/x"}, RestCfg{},
                TlsCfg{.enabled = false, .cert_path = {}, .key_path = {}, .ca_path = {}},
                AuthCfg{.users = std::move(users)}, LogCfg{}, RetentionCfg{}, std::move(peers));
}

// `accept_token` is what the peer presents to us; the rest is unused here.
PeerCfg peer(std::string name, std::string accept_token) {
  return {.name = std::move(name),
          .host = "localhost",
          .port = 8080,
          .token = "unused",
          .accept_token = std::move(accept_token)};
}

std::string basic(std::string_view user_colon_password) {
  return "Basic "
         + base64_encode(ByteSpan(reinterpret_cast<const std::byte*>(user_colon_password.data()),
                                  user_colon_password.size()));
}

class AuthenticatorTest : public ::testing::Test {
protected:
  AuthenticatorTest()
      : cfg_(make_config({{.name = "alice", .password_hash = hash_of("a:pw")},
                          {.name = "bob", .password_hash = hash_of("bob-pw")}},
                         {peer("siteB", "token-from-B"), peer("siteC", "token-from-C"),
                          peer("siteD", "")}))
      , directory_(LocalDirectory::create(cfg_).value())
      , auth_(Authenticator::create(cfg_, *directory_).value()) {}

  void expect_denied(const Result<std::string>& r) {
    ASSERT_FALSE(r.has_value()) << *r;
    EXPECT_EQ(r.error().code, ErrCode::auth);
  }

  Config cfg_;
  std::unique_ptr<LocalDirectory> directory_;
  Authenticator auth_;
};

// Records what the Authenticator hands over and answers as configured.
class FakeDirectory final : public IUserDirectory {
public:
  explicit FakeDirectory(Result<std::string> answer) : answer_(std::move(answer)) {}

  Result<std::string> verify(std::string_view user, std::string_view password) const override {
    last_user = user;
    last_password = password;
    return answer_;
  }
  Result<bool> has_user(std::string_view /*name*/) const override { return false; }
  Result<bool> has_group(std::string_view /*name*/) const override { return false; }
  bool is_member(std::string_view /*user*/, std::string_view /*group*/) const override {
    return false;
  }

  mutable std::string last_user;
  mutable std::string last_password;

private:
  Result<std::string> answer_;
};

}  // namespace

TEST_F(AuthenticatorTest, AcceptsValidBasicCredentials) {
  EXPECT_EQ(auth_.authenticate_user(basic("bob:bob-pw")).value(), "bob");
  // The password may contain ':'; only the first one separates the name.
  EXPECT_EQ(auth_.authenticate_user(basic("alice:a:pw")).value(), "alice");
  // Scheme is case-insensitive, extra spaces are allowed.
  EXPECT_EQ(auth_.authenticate_user("basic   " + basic("bob:bob-pw").substr(6)).value(), "bob");
}

TEST_F(AuthenticatorTest, RejectsWrongPasswordAndUnknownUserAlike) {
  auto wrong = auth_.authenticate_user(basic("bob:nope"));
  auto unknown = auth_.authenticate_user(basic("carol:bob-pw"));
  expect_denied(wrong);
  expect_denied(unknown);
  EXPECT_EQ(wrong.error().detail, unknown.error().detail);
}

TEST_F(AuthenticatorTest, CachedPasswordDoesNotLetOthersIn) {
  ASSERT_TRUE(auth_.authenticate_user(basic("bob:bob-pw")).has_value());
  ASSERT_TRUE(auth_.authenticate_user(basic("bob:bob-pw")).has_value());  // cached
  expect_denied(auth_.authenticate_user(basic("bob:nope")));
  expect_denied(auth_.authenticate_user(basic("alice:bob-pw")));
}

TEST_F(AuthenticatorTest, RejectsMalformedUserHeaders) {
  expect_denied(auth_.authenticate_user(""));
  expect_denied(auth_.authenticate_user("Basic"));
  expect_denied(auth_.authenticate_user("Basic !!!!"));
  expect_denied(auth_.authenticate_user(basic("no-colon")));
  expect_denied(auth_.authenticate_user("Bearer token-from-B"));
}

TEST_F(AuthenticatorTest, AcceptsPeerTokens) {
  EXPECT_EQ(auth_.authenticate_peer("Bearer token-from-B").value(), "siteB");
  EXPECT_EQ(auth_.authenticate_peer("bearer token-from-C").value(), "siteC");
}

TEST_F(AuthenticatorTest, RejectsBadPeerTokens) {
  expect_denied(auth_.authenticate_peer(""));
  expect_denied(auth_.authenticate_peer("Bearer "));
  expect_denied(auth_.authenticate_peer("Bearer token-from-X"));
  expect_denied(auth_.authenticate_peer("Bearer token-from-B "));
  expect_denied(auth_.authenticate_peer(basic("bob:bob-pw")));
  // A user's credentials never open /internal/*.
  expect_denied(auth_.authenticate_peer("Bearer bob-pw"));
}

TEST(Authenticator, BadPasswordHashFailsAtStartup) {
  auto r = LocalDirectory::create(make_config({{.name = "alice", .password_hash = "<hash>"}}));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrCode::validation);
  EXPECT_NE(r.error().detail.find("alice"), std::string::npos);
}

TEST(Authenticator, WorksWithoutUsers) {
  const auto cfg = make_config({});
  auto directory = LocalDirectory::create(cfg);
  ASSERT_TRUE(directory.has_value());
  auto auth = Authenticator::create(cfg, **directory);
  ASSERT_TRUE(auth.has_value());
  auto r = auth->authenticate_user(basic("bob:pw"));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrCode::auth);
}

TEST(Authenticator, HandsDecodedCredentialsToTheDirectory) {
  const FakeDirectory directory(std::string("Alice"));
  auto auth = Authenticator::create(make_config({}), directory).value();
  // The directory's spelling of the name wins over what was typed.
  EXPECT_EQ(auth.authenticate_user(basic("alice:p:w")).value(), "Alice");
  EXPECT_EQ(directory.last_user, "alice");
  EXPECT_EQ(directory.last_password, "p:w");
}

TEST(Authenticator, DirectoryOutageIsNotAnAuthError) {
  const FakeDirectory directory(
      std::unexpected(Error::make(ErrCode::unavailable, "user directory unavailable")));
  auto auth = Authenticator::create(make_config({}), directory).value();
  auto r = auth.authenticate_user(basic("alice:pw"));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrCode::unavailable);
}

TEST(Authenticator, PeersDoNotDependOnTheDirectory) {
  const FakeDirectory directory(
      std::unexpected(Error::make(ErrCode::unavailable, "user directory unavailable")));
  auto auth =
      Authenticator::create(make_config({}, {peer("siteB", "token-from-B")}), directory).value();
  EXPECT_EQ(auth.authenticate_peer("Bearer token-from-B").value(), "siteB");
}
