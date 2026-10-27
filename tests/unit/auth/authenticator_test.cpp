#include <auth/authenticator.hpp>
#include <auth/local_directory.hpp>
#include <auth/password.hpp>
#include <common/base64.hpp>
#include <config/config.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

using confide::auth::Authenticator;
using confide::auth::IUserDirectory;
using confide::auth::LocalDirectory;
using confide::auth::PasswordHash;
using confide::auth::Scope;
using confide::common::base64_encode;
using confide::common::ByteSpan;
using confide::common::ErrCode;
using confide::common::Error;
using confide::common::IClock;
using confide::common::Result;
using confide::common::Timestamp;
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

constexpr std::string_view kSource = "10.0.0.1";

// 2026-09-24T12:00:00Z; moved by hand.
class FakeClock final : public IClock {
public:
  Timestamp now() const override { return now_; }
  void advance(std::chrono::seconds d) { now_ += d; }

private:
  Timestamp now_ = std::chrono::sys_days{std::chrono::year{2026} / 9 / 24} + std::chrono::hours{12};
};

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
      : cfg_(make_config(
            {{.name = "alice", .password_hash = hash_of("a:pw")},
             {.name = "bob", .password_hash = hash_of("bob-pw")}},
            {peer("siteB", "token-from-B"), peer("siteC", "token-from-C"), peer("siteD", "")}))
      , directory_(LocalDirectory::create(cfg_).value())
      , auth_(Authenticator::create(cfg_, *directory_, clock_).value()) {}

  void expect_denied(const Result<std::string>& r) {
    ASSERT_FALSE(r.has_value()) << *r;
    EXPECT_EQ(r.error().code, ErrCode::auth);
  }

  Config cfg_;
  std::unique_ptr<LocalDirectory> directory_;
  FakeClock clock_;
  Authenticator auth_;
};

// Records what the Authenticator hands over and answers as configured.
class FakeDirectory final : public IUserDirectory {
public:
  explicit FakeDirectory(Result<std::string> answer) : answer_(std::move(answer)) {}

  Result<std::string> verify(std::string_view user, std::string_view password) const override {
    ++calls;
    last_user = user;
    last_password = password;
    return answer_;
  }
  Result<bool> has_user(std::string_view /*name*/) const override { return false; }
  Result<bool> has_group(std::string_view /*name*/) const override { return false; }
  bool is_member(std::string_view /*user*/, std::string_view /*group*/) const override {
    return false;
  }

  mutable int calls = 0;
  mutable std::string last_user;
  mutable std::string last_password;

private:
  Result<std::string> answer_;
};

}  // namespace

TEST_F(AuthenticatorTest, AcceptsValidBasicCredentials) {
  EXPECT_EQ(auth_.authenticate_user(basic("bob:bob-pw"), kSource).value(), "bob");
  // The password may contain ':'; only the first one separates the name.
  EXPECT_EQ(auth_.authenticate_user(basic("alice:a:pw"), kSource).value(), "alice");
  // Scheme is case-insensitive, extra spaces are allowed.
  EXPECT_EQ(auth_.authenticate_user("basic   " + basic("bob:bob-pw").substr(6), kSource).value(),
            "bob");
}

TEST_F(AuthenticatorTest, RejectsWrongPasswordAndUnknownUserAlike) {
  auto wrong = auth_.authenticate_user(basic("bob:nope"), kSource);
  auto unknown = auth_.authenticate_user(basic("carol:bob-pw"), kSource);
  expect_denied(wrong);
  expect_denied(unknown);
  EXPECT_EQ(wrong.error().detail, unknown.error().detail);
}

TEST_F(AuthenticatorTest, CachedPasswordDoesNotLetOthersIn) {
  ASSERT_TRUE(auth_.authenticate_user(basic("bob:bob-pw"), kSource).has_value());
  ASSERT_TRUE(auth_.authenticate_user(basic("bob:bob-pw"), kSource).has_value());  // cached
  expect_denied(auth_.authenticate_user(basic("bob:nope"), kSource));
  expect_denied(auth_.authenticate_user(basic("alice:bob-pw"), kSource));
}

TEST_F(AuthenticatorTest, RejectsMalformedUserHeaders) {
  expect_denied(auth_.authenticate_user("", kSource));
  expect_denied(auth_.authenticate_user("Basic", kSource));
  expect_denied(auth_.authenticate_user("Basic !!!!", kSource));
  expect_denied(auth_.authenticate_user(basic("no-colon"), kSource));
  expect_denied(auth_.authenticate_user("Bearer token-from-B", kSource));
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
  const FakeClock clock;
  auto auth = Authenticator::create(cfg, **directory, clock);
  ASSERT_TRUE(auth.has_value());
  auto r = auth->authenticate_user(basic("bob:pw"), kSource);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrCode::auth);
}

TEST(Authenticator, HandsDecodedCredentialsToTheDirectory) {
  const FakeClock clock;
  const FakeDirectory directory(std::string("Alice"));
  auto auth = Authenticator::create(make_config({}), directory, clock).value();
  // The directory's spelling of the name wins over what was typed.
  EXPECT_EQ(auth.authenticate_user(basic("alice:p:w"), kSource).value(), "Alice");
  EXPECT_EQ(directory.last_user, "alice");
  EXPECT_EQ(directory.last_password, "p:w");
}

TEST(Authenticator, DirectoryOutageIsNotAnAuthError) {
  const FakeClock clock;
  const FakeDirectory directory(
      std::unexpected(Error::make(ErrCode::unavailable, "user directory unavailable")));
  auto auth = Authenticator::create(make_config({}), directory, clock).value();
  auto r = auth.authenticate_user(basic("alice:pw"), kSource);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrCode::unavailable);
}

TEST(Authenticator, PeersDoNotDependOnTheDirectory) {
  const FakeClock clock;
  const FakeDirectory directory(
      std::unexpected(Error::make(ErrCode::unavailable, "user directory unavailable")));
  auto auth =
      Authenticator::create(make_config({}, {peer("siteB", "token-from-B")}), directory, clock)
          .value();
  EXPECT_EQ(auth.authenticate_peer("Bearer token-from-B").value(), "siteB");
}

// --- User tokens -------------------------------------------------------------

TEST_F(AuthenticatorTest, TokenStandsInForThePassword) {
  const auto issued = auth_.issue_token("bob").value();
  EXPECT_TRUE(issued.token.starts_with("ftu_"));
  EXPECT_EQ(issued.token.size(), 4U + 64U);
  EXPECT_EQ(auth_.authenticate_user("Bearer " + issued.token, kSource).value(), "bob");
  // Each login gets its own token.
  EXPECT_NE(auth_.issue_token("bob").value().token, issued.token);
}

TEST_F(AuthenticatorTest, TokenCanNotBuyAnotherToken) {
  const auto issued = auth_.issue_token("bob").value();
  expect_denied(auth_.authenticate_password("Bearer " + issued.token, kSource));
  EXPECT_EQ(auth_.authenticate_password(basic("bob:bob-pw"), kSource).value(), "bob");
}

TEST_F(AuthenticatorTest, UserAndPeerTokensDoNotCross) {
  const auto issued = auth_.issue_token("bob").value();
  expect_denied(auth_.authenticate_peer("Bearer " + issued.token));
  expect_denied(auth_.authenticate_user("Bearer token-from-B", kSource));
}

TEST_F(AuthenticatorTest, RevokedTokenIsRejected) {
  const auto issued = auth_.issue_token("bob").value();
  auth_.revoke_token("Bearer " + issued.token);
  expect_denied(auth_.authenticate_user("Bearer " + issued.token, kSource));
  auth_.revoke_token("Bearer ftu_unknown");  // ignored
  auth_.revoke_token(basic("bob:bob-pw"));   // ignored
}

TEST_F(AuthenticatorTest, TokenExpiresAfterTtlAndIdleTime) {
  // Defaults: token_ttl_s = 3600, token_idle_s = 900.
  const auto busy = auth_.issue_token("bob").value();
  const auto idle = auth_.issue_token("bob").value();
  for (int i = 0; i < 4; ++i) {
    clock_.advance(std::chrono::seconds{800});
    EXPECT_TRUE(auth_.authenticate_user("Bearer " + busy.token, kSource).has_value()) << i;
  }
  expect_denied(auth_.authenticate_user("Bearer " + idle.token, kSource));  // idle > 900 s
  clock_.advance(std::chrono::seconds{400});                                // 3600 s in all
  expect_denied(auth_.authenticate_user("Bearer " + busy.token, kSource));
}

TEST_F(AuthenticatorTest, WrongPasswordsAreThrottled) {
  for (int i = 0; i < 5; ++i) {
    expect_denied(auth_.authenticate_password(basic("bob:nope"), kSource));
  }
  // Sixth failure starts the backoff; then even the right password waits.
  expect_denied(auth_.authenticate_password(basic("bob:nope"), kSource));
  auto r = auth_.authenticate_password(basic("bob:bob-pw"), kSource);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrCode::throttled);
  // Basic on ordinary routes is throttled too, or it would be a way around.
  EXPECT_EQ(auth_.authenticate_user(basic("bob:bob-pw"), kSource).error().code, ErrCode::throttled);
  // Other users from other addresses are not affected.
  EXPECT_EQ(auth_.authenticate_password(basic("alice:a:pw"), "10.0.0.2").value(), "alice");

  clock_.advance(std::chrono::seconds{30});
  EXPECT_EQ(auth_.authenticate_password(basic("bob:bob-pw"), kSource).value(), "bob");
}

TEST(Authenticator, ThrottledGuessesNeverReachTheDirectory) {
  const FakeClock clock;
  const FakeDirectory directory(std::unexpected(Error::make(ErrCode::auth, "bad credentials")));
  auto auth = Authenticator::create(make_config({}), directory, clock).value();
  for (int i = 0; i < 20; ++i) {
    (void)auth.authenticate_password(basic("alice:guess"), kSource);
  }
  EXPECT_EQ(directory.calls, 6);  // 5 free + the one that started the backoff
}

TEST(Authenticator, DirectoryOutageDoesNotCountAsFailure) {
  const FakeClock clock;
  const FakeDirectory directory(
      std::unexpected(Error::make(ErrCode::unavailable, "user directory unavailable")));
  auto auth = Authenticator::create(make_config({}), directory, clock).value();
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(auth.authenticate_password(basic("alice:pw"), kSource).error().code,
              ErrCode::unavailable);
  }
}

// --- Admin ------------------------------------------------------------------

namespace {

// alice is in "admins", bob is not; admin tokens last 600 s.
Config admin_config() {
  return Config("siteA", StorageCfg{.root = "/tmp/x"}, RestCfg{},
                TlsCfg{.enabled = false, .cert_path = {}, .key_path = {}, .ca_path = {}},
                AuthCfg{.users = {{.name = "alice", .password_hash = hash_of("a-pw")},
                                  {.name = "bob", .password_hash = hash_of("b-pw")}},
                        .groups = {{.name = "admins", .members = {"alice"}}},
                        .admin_group = "admins",
                        .admin_token_ttl_s = 600,
                        .admin_token_idle_s = 300},
                LogCfg{}, RetentionCfg{});
}

class AdminAuthTest : public ::testing::Test {
protected:
  AdminAuthTest()
      : cfg_(admin_config())
      , directory_(LocalDirectory::create(cfg_).value())
      , auth_(Authenticator::create(cfg_, *directory_, clock_).value()) {}

  Config cfg_;
  std::unique_ptr<LocalDirectory> directory_;
  FakeClock clock_;
  Authenticator auth_;
};

}  // namespace

TEST_F(AdminAuthTest, MembersOfTheAdminGroupAreAdmins) {
  EXPECT_EQ(auth_.authenticate_admin(basic("alice:a-pw"), kSource).value(), "alice");
  EXPECT_EQ(auth_.authenticate_admin_password(basic("alice:a-pw"), kSource).value(), "alice");
}

TEST_F(AdminAuthTest, OtherUsersAreForbidden) {
  auto r = auth_.authenticate_admin(basic("bob:b-pw"), kSource);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrCode::forbidden);
  // A wrong password is still 401, not 403: nothing is told about rights.
  EXPECT_EQ(auth_.authenticate_admin(basic("alice:nope"), kSource).error().code, ErrCode::auth);
}

TEST_F(AdminAuthTest, OnlyAdminTokensOpenAdminRoutes) {
  const auto user = auth_.issue_token("alice").value();
  const auto admin = auth_.issue_token("alice", Scope::admin).value();
  EXPECT_EQ(admin.expires_at, clock_.now() + std::chrono::seconds{600});

  EXPECT_EQ(auth_.authenticate_admin("Bearer " + admin.token, kSource).value(), "alice");
  EXPECT_EQ(auth_.authenticate_admin("Bearer " + user.token, kSource).error().code, ErrCode::auth);
  // ...and admin tokens don't open user routes or the token endpoint.
  EXPECT_EQ(auth_.authenticate_user("Bearer " + admin.token, kSource).error().code, ErrCode::auth);
  EXPECT_EQ(auth_.authenticate_admin_password("Bearer " + admin.token, kSource).error().code,
            ErrCode::auth);
  // Never mistaken for a peer token either.
  EXPECT_EQ(auth_.authenticate_peer("Bearer " + admin.token).error().code, ErrCode::auth);
}

TEST_F(AdminAuthTest, AdminTokenOfANonMemberIsForbidden) {
  // Membership is checked on each request, not when the token was issued.
  const auto admin = auth_.issue_token("bob", Scope::admin).value();
  EXPECT_EQ(auth_.authenticate_admin("Bearer " + admin.token, kSource).error().code,
            ErrCode::forbidden);
}

TEST_F(AdminAuthTest, SessionsCanBeListedAndRevoked) {
  const auto bob = auth_.issue_token("bob").value();
  ASSERT_TRUE(auth_.issue_token("alice", Scope::admin).has_value());
  EXPECT_EQ(auth_.sessions().size(), 2U);
  EXPECT_EQ(auth_.revoke_user("bob"), 1U);
  EXPECT_EQ(auth_.authenticate_user("Bearer " + bob.token, kSource).error().code, ErrCode::auth);
  EXPECT_EQ(auth_.sessions().size(), 1U);
}

TEST(Authenticator, NoAdminGroupMeansNoAdmins) {
  const FakeClock clock;
  const FakeDirectory directory(std::string{"alice"});
  auto auth = Authenticator::create(make_config({}), directory, clock).value();
  EXPECT_EQ(auth.authenticate_admin(basic("alice:pw"), kSource).error().code, ErrCode::forbidden);
}

TEST(Authenticator, UnreachableDirectoryGivesNoAdminRights) {
  // FakeDirectory::is_member is always false, as LDAP's is when it is down.
  const FakeClock clock;
  const FakeDirectory directory(std::string{"alice"});
  auto auth = Authenticator::create(admin_config(), directory, clock).value();
  EXPECT_EQ(auth.authenticate_admin(basic("alice:pw"), kSource).error().code, ErrCode::forbidden);
}
