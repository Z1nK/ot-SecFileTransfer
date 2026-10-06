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
  const fs::path dir = fs::temp_directory_path() / "confide_config_test";
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
