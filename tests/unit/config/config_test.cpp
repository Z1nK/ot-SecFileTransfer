#include <config/config.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using confide::common::ErrCode;
using confide::config::ConfigLoader;
using confide::logging::LogLevel;

namespace fs = std::filesystem;

namespace {

// Smallest config that loads: TLS off so no cert/key are needed.
constexpr std::string_view kMinimal = R"(
instance_name = "siteA"
[storage]
root = "/srv/confide"
[tls]
enabled = false
)";

std::string with(std::string_view extra) { return std::string(kMinimal) + std::string(extra); }

void expect_invalid(std::string_view toml, std::string_view detail_part) {
  auto cfg = ConfigLoader::from_string(toml);
  ASSERT_FALSE(cfg.has_value()) << toml;
  EXPECT_EQ(cfg.error().code, ErrCode::validation);
  EXPECT_NE(cfg.error().detail.find(detail_part), std::string::npos) << cfg.error().detail;
}

}  // namespace

TEST(ConfigLoader, MinimalUsesDefaults) {
  auto cfg = ConfigLoader::from_string(kMinimal);
  ASSERT_TRUE(cfg.has_value()) << cfg.error().detail;
  EXPECT_EQ(cfg->instance_name(), "siteA");
  EXPECT_EQ(cfg->storage().root, "/srv/confide");
  EXPECT_EQ(cfg->rest().bind, "0.0.0.0");
  EXPECT_EQ(cfg->rest().port, 8080);
  EXPECT_FALSE(cfg->tls().enabled);
  EXPECT_EQ(cfg->log().level, LogLevel::info);
  EXPECT_EQ(cfg->log().dir, fs::path("/srv/confide/logs"));
  EXPECT_EQ(cfg->retention().default_days, 30U);
  EXPECT_TRUE(cfg->peers().empty());
  EXPECT_TRUE(cfg->auth().users.empty());
}

TEST(ConfigLoader, FullConfig) {
  auto cfg = ConfigLoader::from_string(R"(
instance_name = "siteA"

[storage]
root = "/srv/confide"

[rest]
bind = "127.0.0.1"
port = 9000
threads = 8
request_timeout_ms = 5000

[tls]
cert = "/etc/confide/cert.pem"
key = "/etc/confide/key.pem"

[auth]
token_ttl_s = 600

[[user]]
name = "alice"
password_hash = "h1"

[[user]]
name = "bob"
password_hash = "h2"

[log]
level = "debug"
dir = "/var/log/confide"
rotate_bytes = 1024

[retention]
default_days = 7
max_days = 30
sweep_interval_s = 60

[[peer]]
name = "siteB"
host = "b.example"
port = 8443
token = "out"
accept_token = "in"
)");
  ASSERT_TRUE(cfg.has_value()) << cfg.error().detail;
  EXPECT_EQ(cfg->rest().bind, "127.0.0.1");
  EXPECT_EQ(cfg->rest().port, 9000);
  EXPECT_EQ(cfg->rest().threads, 8U);
  EXPECT_EQ(cfg->rest().request_timeout_ms, 5000U);
  EXPECT_TRUE(cfg->tls().enabled);
  EXPECT_EQ(cfg->tls().cert_path, "/etc/confide/cert.pem");
  EXPECT_EQ(cfg->auth().token_ttl_s, 600U);
  ASSERT_EQ(cfg->auth().users.size(), 2U);
  ASSERT_NE(cfg->find_user("bob"), nullptr);
  EXPECT_EQ(cfg->find_user("bob")->password_hash, "h2");
  EXPECT_EQ(cfg->find_user("carol"), nullptr);
  EXPECT_EQ(cfg->log().level, LogLevel::debug);
  EXPECT_EQ(cfg->log().dir, "/var/log/confide");
  EXPECT_EQ(cfg->log().rotate_bytes, 1024U);
  EXPECT_EQ(cfg->retention().max_days, 30U);
  const auto* peer = cfg->find_peer("siteB");
  ASSERT_NE(peer, nullptr);
  EXPECT_EQ(peer->host, "b.example");
  EXPECT_EQ(peer->port, 8443);
  EXPECT_EQ(peer->token, "out");
  EXPECT_EQ(peer->accept_token, "in");
  EXPECT_EQ(cfg->find_peer("siteC"), nullptr);
  EXPECT_TRUE(cfg->warnings().empty());
}

TEST(ConfigLoader, SyntaxErrorIsValidation) {
  expect_invalid("instance_name = ", "");
}

TEST(ConfigLoader, RequiredFields) {
  expect_invalid("[storage]\nroot = \"/x\"\n[tls]\nenabled = false\n", "instance_name");
  expect_invalid("instance_name = \"a\"\n[tls]\nenabled = false\n", "[storage]");
  expect_invalid("instance_name = \"a\"\n[storage]\n[tls]\nenabled = false\n", "storage.root");
}

TEST(ConfigLoader, TlsOnByDefaultNeedsCertAndKey) {
  expect_invalid("instance_name = \"a\"\n[storage]\nroot = \"/x\"\n", "tls.cert");
}

TEST(ConfigLoader, TlsOffWarns) {
  auto cfg = ConfigLoader::from_string(kMinimal);
  ASSERT_TRUE(cfg.has_value());
  ASSERT_FALSE(cfg->warnings().empty());
  EXPECT_NE(cfg->warnings().front().find("tls.enabled"), std::string::npos);
}

TEST(ConfigLoader, TypeAndRangeErrors) {
  expect_invalid(with("[rest]\nport = \"8080\"\n"), "rest.port: expected an integer");
  expect_invalid(with("[rest]\nport = 70000\n"), "rest.port");
  expect_invalid(with("[rest]\nport = 0\n"), "rest.port");
  expect_invalid(with("[rest]\nthreads = -1\n"), "rest.threads");
  expect_invalid(with("[log]\nlevel = \"verbose\"\n"), "log.level");
  expect_invalid(with("[retention]\ndefault_days = 40\nmax_days = 30\n"), "retention.max_days");
  expect_invalid(with("[retention]\ndefault_days = 0\n"), "retention.default_days");
  expect_invalid("instance_name = \"a\"\nstorage = 5\n", "storage: expected a table");
}

TEST(ConfigLoader, InvalidNames) {
  expect_invalid(
      "instance_name = \"site@A\"\n[storage]\nroot = \"/x\"\n[tls]\nenabled = false\n",
      "instance_name");
  expect_invalid(with("[[user]]\nname = \"a:b\"\npassword_hash = \"h\"\n"), "user[0].name");
}

TEST(ConfigLoader, UserRules) {
  expect_invalid(with("[[user]]\nname = \"alice\"\npassword = \"secret\"\n"), "plaintext");
  expect_invalid(with("[[user]]\nname = \"alice\"\n"), "password_hash is required");
  expect_invalid(with("[[user]]\nname = \"alice\"\npassword_hash = \"h\"\n"
                      "[[user]]\nname = \"alice\"\npassword_hash = \"h\"\n"),
                 "duplicate user");
}

TEST(ConfigLoader, PeerRules) {
  const std::string ok = "host = \"h\"\ntoken = \"t1\"\naccept_token = \"t2\"\n";
  expect_invalid(with("[[peer]]\nname = \"siteA\"\n" + ok), "own name");
  expect_invalid(with("[[peer]]\nname = \"b\"\n" + ok + "[[peer]]\nname = \"b\"\n" + ok),
                 "duplicate peer");
  expect_invalid(with("[[peer]]\nname = \"b\"\ntoken = \"t1\"\naccept_token = \"t2\"\n"),
                 "host is required");
  expect_invalid(with("[[peer]]\nname = \"b\"\nhost = \"h\"\ntoken = \"t1\"\n"),
                 "accept_token");
  expect_invalid(
      with("[[peer]]\nname = \"b\"\nhost = \"h\"\ntoken = \"same\"\naccept_token = \"same\"\n"),
      "must differ");
}

TEST(ConfigLoader, UnknownKeysWarn) {
  auto cfg = ConfigLoader::from_string(with("[rest]\nprot = 1\n"));
  ASSERT_TRUE(cfg.has_value()) << cfg.error().detail;
  bool found = false;
  for (const auto& w : cfg->warnings()) {
    found = found || w.find("rest.prot") != std::string::npos;
  }
  EXPECT_TRUE(found);
}

TEST(ConfigLoader, FromFileMissingIsIo) {
  auto cfg = ConfigLoader::from_file("/nonexistent/confide.toml");
  ASSERT_FALSE(cfg.has_value());
  EXPECT_EQ(cfg.error().code, ErrCode::io);
}

TEST(ConfigLoader, FromFileResolvesRelativePaths) {
  const fs::path dir = fs::temp_directory_path()
      / (std::string{"confide_config_test_"}
         + ::testing::UnitTest::GetInstance()->current_test_info()->name());
  fs::create_directories(dir);
  const fs::path file = dir / "server.toml";
  {
    std::ofstream out(file);
    out << "instance_name = \"siteA\"\n[storage]\nroot = \"data\"\n[tls]\nenabled = false\n"
           "[log]\ndir = \"/abs/logs\"\n";
  }
  auto cfg = ConfigLoader::from_file(file);
  fs::remove_all(dir);
  ASSERT_TRUE(cfg.has_value()) << cfg.error().detail;
  EXPECT_EQ(cfg->storage().root, fs::absolute(dir) / "data");
  EXPECT_EQ(cfg->log().dir, "/abs/logs");
}

namespace {

// A config file plus a users file in a fresh temp folder; removed on scope exit.
struct UsersFixture {
  fs::path dir = fs::temp_directory_path()
      / (std::string{"confide_users_test_"}
         + ::testing::UnitTest::GetInstance()->current_test_info()->name());
  fs::path config = dir / "server.toml";
  fs::path users = dir / "users";

  UsersFixture(std::string_view users_text, std::string_view extra_toml = "",
               fs::perms mode = fs::perms::owner_read | fs::perms::owner_write) {
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(config) << "instance_name = \"siteA\"\n[storage]\nroot = \"data\"\n"
                             "[tls]\nenabled = false\n[auth]\nusers_file = \"users\"\n"
                          << extra_toml;
    std::ofstream(users) << users_text;
    fs::permissions(users, mode);
  }
  ~UsersFixture() { fs::remove_all(dir); }
  UsersFixture(const UsersFixture&) = delete;
  UsersFixture& operator=(const UsersFixture&) = delete;
};

bool has_warning(const confide::config::Config& cfg, std::string_view part) {
  for (const auto& w : cfg.warnings()) {
    if (w.find(part) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(ConfigLoader, UsersFileLoads) {
  UsersFixture f("# name:password_hash\n\nalice:h1\r\n  bob:h2  \n");
  auto cfg = ConfigLoader::from_file(f.config);
  ASSERT_TRUE(cfg.has_value()) << cfg.error().detail;
  EXPECT_EQ(cfg->auth().users_file, fs::absolute(f.users));
  ASSERT_EQ(cfg->auth().users.size(), 2U);
  ASSERT_NE(cfg->find_user("alice"), nullptr);
  EXPECT_EQ(cfg->find_user("alice")->password_hash, "h1");
  ASSERT_NE(cfg->find_user("bob"), nullptr);
  EXPECT_EQ(cfg->find_user("bob")->password_hash, "h2");
  EXPECT_FALSE(has_warning(*cfg, "users_file"));
  EXPECT_FALSE(has_warning(*cfg, "no users"));
}

TEST(ConfigLoader, UsersFileMergesWithInlineUsers) {
  UsersFixture f("alice:h1\n", "[[user]]\nname = \"bob\"\npassword_hash = \"h2\"\n");
  auto cfg = ConfigLoader::from_file(f.config);
  ASSERT_TRUE(cfg.has_value()) << cfg.error().detail;
  EXPECT_EQ(cfg->auth().users.size(), 2U);
  EXPECT_NE(cfg->find_user("alice"), nullptr);
  EXPECT_NE(cfg->find_user("bob"), nullptr);
}

TEST(ConfigLoader, UsersFileRules) {
  auto expect_bad = [](std::string_view users, std::string_view extra, std::string_view part) {
    UsersFixture f(users, extra);
    auto cfg = ConfigLoader::from_file(f.config);
    ASSERT_FALSE(cfg.has_value()) << users;
    EXPECT_EQ(cfg.error().code, ErrCode::validation);
    EXPECT_NE(cfg.error().detail.find(part), std::string::npos) << cfg.error().detail;
  };
  expect_bad("alice:h1\nbob\n", "", "users:2: expected 'name:password_hash'");
  expect_bad("a@b:h1\n", "", "users:1.name");
  expect_bad("alice:\n", "", "users:1.password_hash is required");
  expect_bad("alice:h1\nalice:h2\n", "", "duplicate user");
  expect_bad("alice:h1\n", "[[user]]\nname = \"alice\"\npassword_hash = \"h2\"\n",
             "duplicate user");
}

TEST(ConfigLoader, UsersFileMissingIsIo) {
  UsersFixture f("alice:h1\n");
  fs::remove(f.users);
  auto cfg = ConfigLoader::from_file(f.config);
  ASSERT_FALSE(cfg.has_value());
  EXPECT_EQ(cfg.error().code, ErrCode::io);
}

TEST(ConfigLoader, UsersFileReadableByOthersWarns) {
  UsersFixture f("alice:h1\n", "",
                 fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read
                     | fs::perms::others_read);
  auto cfg = ConfigLoader::from_file(f.config);
  ASSERT_TRUE(cfg.has_value()) << cfg.error().detail;
  EXPECT_TRUE(has_warning(*cfg, "chmod 600"));
}

TEST(ConfigLoader, UsersFileEmptyWarnsNobodyCanLogIn) {
  UsersFixture f("# nobody yet\n");
  auto cfg = ConfigLoader::from_file(f.config);
  ASSERT_TRUE(cfg.has_value()) << cfg.error().detail;
  EXPECT_TRUE(has_warning(*cfg, "no users configured"));
}

namespace {

// UsersFixture plus a groups file, pointed to by auth.groups_file.
struct GroupsFixture : UsersFixture {
  fs::path groups = dir / "groups";

  explicit GroupsFixture(std::string_view groups_text, std::string_view extra_toml = "")
      : UsersFixture("alice:h1\nbob:h2\ncarol:h3\n",
                     std::string("groups_file = \"groups\"\n") + std::string(extra_toml)) {
    std::ofstream(groups) << groups_text;
  }
};

}  // namespace

TEST(ConfigLoader, GroupsFileLoads) {
  GroupsFixture f("# name:member,...\n\ndevs: alice , bob\r\n  ops:carol\nempty:\n");
  auto cfg = ConfigLoader::from_file(f.config);
  ASSERT_TRUE(cfg.has_value()) << cfg.error().detail;
  EXPECT_EQ(cfg->auth().groups_file, fs::absolute(f.groups));
  ASSERT_EQ(cfg->auth().groups.size(), 3U);
  ASSERT_NE(cfg->find_group("devs"), nullptr);
  EXPECT_EQ(cfg->find_group("devs")->members, (std::vector<std::string>{"alice", "bob"}));
  ASSERT_NE(cfg->find_group("empty"), nullptr);
  EXPECT_TRUE(cfg->find_group("empty")->members.empty());
  EXPECT_EQ(cfg->find_group("alice"), nullptr);

  EXPECT_TRUE(cfg->is_member("alice", "devs"));
  EXPECT_TRUE(cfg->is_member("carol", "ops"));
  EXPECT_FALSE(cfg->is_member("carol", "devs"));
  EXPECT_FALSE(cfg->is_member("alice", "nosuch"));
  EXPECT_FALSE(cfg->is_member("alice", "alice"));
}

TEST(ConfigLoader, GroupsFileMergesWithInlineGroups) {
  GroupsFixture f("devs:alice\n", "[[group]]\nname = \"ops\"\nmembers = [\"bob\", \"carol\"]\n");
  auto cfg = ConfigLoader::from_file(f.config);
  ASSERT_TRUE(cfg.has_value()) << cfg.error().detail;
  EXPECT_EQ(cfg->auth().groups.size(), 2U);
  EXPECT_TRUE(cfg->is_member("alice", "devs"));
  EXPECT_TRUE(cfg->is_member("carol", "ops"));
}

TEST(ConfigLoader, GroupsFileRules) {
  auto expect_bad = [](std::string_view groups, std::string_view extra, std::string_view part) {
    GroupsFixture f(groups, extra);
    auto cfg = ConfigLoader::from_file(f.config);
    ASSERT_FALSE(cfg.has_value()) << groups;
    EXPECT_EQ(cfg.error().code, ErrCode::validation);
    EXPECT_NE(cfg.error().detail.find(part), std::string::npos) << cfg.error().detail;
  };
  expect_bad("devs:alice\nops\n", "", "groups:2: expected 'name:member,member,...'");
  expect_bad("d@v:alice\n", "", "groups:1.name");
  expect_bad("devs:alice\ndevs:bob\n", "", "duplicate group");
  expect_bad("alice:bob\n", "", "already a user name");
  expect_bad("devs:alice,dave\n", "", "'dave' is not a configured user");
  expect_bad("devs:alice,,bob\n", "", "'' is not a configured user");
  expect_bad("devs:alice,bob,alice\n", "", "'alice' is listed twice");
  expect_bad("devs:alice\n", "[[group]]\nname = \"devs\"\n", "duplicate group");
  expect_bad("", "[[group]]\nname = \"ops\"\nmembers = \"bob\"\n", "expected an array of strings");
}

TEST(ConfigLoader, GroupsFileMissingIsIo) {
  GroupsFixture f("devs:alice\n");
  fs::remove(f.groups);
  auto cfg = ConfigLoader::from_file(f.config);
  ASSERT_FALSE(cfg.has_value());
  EXPECT_EQ(cfg.error().code, ErrCode::io);
  EXPECT_NE(cfg.error().detail.find("auth.groups_file"), std::string::npos);
}

namespace {

constexpr std::string_view kLdap = R"(
[auth]
provider = "ldap"
[auth.ldap]
uri = "ldaps://ldap.example.org"
user_base_dn = "ou=people,dc=example,dc=org"
)";

}  // namespace

TEST(ConfigLoader, ProviderIsLocalByDefault) {
  auto cfg = ConfigLoader::from_string(kMinimal);
  ASSERT_TRUE(cfg.has_value()) << cfg.error().detail;
  EXPECT_EQ(cfg->auth().provider, confide::config::AuthProvider::local);
}

TEST(ConfigLoader, LdapProvider) {
  auto cfg = ConfigLoader::from_string(with(std::string(kLdap) + R"(
bind_dn = "cn=reader,dc=example,dc=org"
bind_password = "secret"
group_base_dn = "ou=groups,dc=example,dc=org"
cache_ttl_s = 60
)"));
  ASSERT_TRUE(cfg.has_value()) << cfg.error().detail;
  const auto& auth = cfg->auth();
  EXPECT_EQ(auth.provider, confide::config::AuthProvider::ldap);
  EXPECT_EQ(auth.ldap.preset, "openldap");
  EXPECT_EQ(auth.ldap.uri, "ldaps://ldap.example.org");
  EXPECT_EQ(auth.ldap.user_base_dn, "ou=people,dc=example,dc=org");
  EXPECT_EQ(auth.ldap.group_base_dn, "ou=groups,dc=example,dc=org");
  EXPECT_EQ(auth.ldap.bind_password, "secret");
  EXPECT_EQ(auth.ldap.cache_ttl_s, 60U);
  EXPECT_TRUE(auth.ldap.tls_verify);
  // No local users with LDAP, and no warning about it either.
  EXPECT_FALSE(has_warning(*cfg, "no users"));
}

TEST(ConfigLoader, LdapRules) {
  expect_invalid(with("[auth]\nprovider = \"kerberos\"\n"), "auth.provider");
  expect_invalid(with("[auth]\nprovider = \"ldap\"\n"), "[auth.ldap] section is required");
  expect_invalid(with("[auth]\nprovider = \"ldap\"\n[auth.ldap]\nuser_base_dn = \"x\"\n"),
                 "auth.ldap.uri is required");
  expect_invalid(with("[auth]\nprovider = \"ldap\"\n[auth.ldap]\nuri = \"ldaps://h\"\n"),
                 "auth.ldap.user_base_dn is required");
  expect_invalid(with(std::string(kLdap) + "preset = \"novell\"\n"), "auth.ldap.preset");
  expect_invalid(with(std::string(kLdap) + "timeout_s = 0\n"), "auth.ldap.timeout_s");
  expect_invalid(
      with(std::string(kLdap) + "bind_password = \"a\"\nbind_password_file = \"/x\"\n"),
      "not both");
}

TEST(ConfigLoader, LdapWarnings) {
  auto cfg = ConfigLoader::from_string(with(R"(
[auth]
provider = "ldap"
users_file = "users"
[auth.ldap]
uri = "ldap://ldap.example.org"
user_base_dn = "ou=people,dc=example,dc=org"
tls_verify = false
[[user]]
name = "alice"
password_hash = "h1"
[[group]]
name = "ops"
members = ["alice"]
)"));
  ASSERT_TRUE(cfg.has_value()) << cfg.error().detail;
  // Local users are not loaded, even from a users_file that doesn't exist.
  EXPECT_TRUE(cfg->auth().users.empty());
  EXPECT_TRUE(cfg->auth().groups.empty());
  EXPECT_TRUE(has_warning(*cfg, "[[user]]: ignored"));
  EXPECT_TRUE(has_warning(*cfg, "[[group]]: ignored"));
  EXPECT_TRUE(has_warning(*cfg, "tls_verify = false"));
  EXPECT_TRUE(has_warning(*cfg, "cleartext"));
}

TEST(ConfigLoader, LdapSectionIgnoredWithLocal) {
  auto cfg = ConfigLoader::from_string(with("[auth.ldap]\nuri = \"ldap://h\"\n"));
  ASSERT_TRUE(cfg.has_value()) << cfg.error().detail;
  EXPECT_TRUE(has_warning(*cfg, "[auth.ldap]: ignored"));
}

TEST(ConfigLoader, LdapBindPasswordFile) {
  const fs::path dir = fs::temp_directory_path()
      / (std::string{"confide_ldap_test_"}
         + ::testing::UnitTest::GetInstance()->current_test_info()->name());
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path secret = dir / "ldap_bind";
  std::ofstream(secret) << "s3cret\n";
  fs::permissions(secret, fs::perms::owner_read | fs::perms::owner_write);
  std::ofstream(dir / "server.toml") << kMinimal << kLdap << "bind_password_file = \"ldap_bind\"\n";

  auto cfg = ConfigLoader::from_file(dir / "server.toml");
  ASSERT_TRUE(cfg.has_value()) << cfg.error().detail;
  EXPECT_EQ(cfg->auth().ldap.bind_password, "s3cret");
  EXPECT_FALSE(has_warning(*cfg, "chmod 600"));

  fs::permissions(secret, fs::perms::owner_read | fs::perms::group_read);
  cfg = ConfigLoader::from_file(dir / "server.toml");
  ASSERT_TRUE(cfg.has_value()) << cfg.error().detail;
  EXPECT_TRUE(has_warning(*cfg, "chmod 600"));

  fs::remove(secret);
  cfg = ConfigLoader::from_file(dir / "server.toml");
  ASSERT_FALSE(cfg.has_value());
  EXPECT_EQ(cfg.error().code, ErrCode::io);
  fs::remove_all(dir);
}
