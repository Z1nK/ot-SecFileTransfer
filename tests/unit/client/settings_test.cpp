#include <client/settings.hpp>
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace confide::client {
namespace {

namespace fs = std::filesystem;

Result<LoadedSettings> parse(const std::string& text, const fs::path& base = "/etc/ftc") {
  std::istringstream in{text};
  return parse_settings(in, "test.toml", base);
}

TEST(Settings, EmptyFileSetsNothing) {
  auto s = parse("");
  ASSERT_TRUE(s) << s.error().detail;
  EXPECT_FALSE(s->values.server);
  EXPECT_FALSE(s->values.user);
  EXPECT_FALSE(s->values.ca_file);
  EXPECT_TRUE(s->warnings.empty());
}

TEST(Settings, ReadsAllKeysWithOrigin) {
  auto s = parse(R"(
server = "https://ftc.example.org:8443"
user = "alice"
ca_file = "/etc/pki/ca.pem"
)");
  ASSERT_TRUE(s) << s.error().detail;
  EXPECT_EQ(s->values.server, "https://ftc.example.org:8443");
  EXPECT_EQ(s->values.user, "alice");
  EXPECT_EQ(s->values.ca_file, fs::path{"/etc/pki/ca.pem"});
  EXPECT_EQ(s->values.from.server, "test.toml");
  EXPECT_EQ(s->values.from.user, "test.toml");
  EXPECT_EQ(s->values.from.ca_file, "test.toml");
  EXPECT_TRUE(s->warnings.empty());
}

TEST(Settings, RelativeCaFileIsAgainstTheFile) {
  auto s = parse(R"(ca_file = "certs/../ca.pem")", "/etc/ftc");
  ASSERT_TRUE(s);
  EXPECT_EQ(s->values.ca_file, fs::path{"/etc/ftc/ca.pem"});
}

TEST(Settings, EmptyValueIsSet) {
  auto s = parse(R"(ca_file = "")");
  ASSERT_TRUE(s);
  ASSERT_TRUE(s->values.ca_file);
  EXPECT_TRUE(s->values.ca_file->empty());
}

TEST(Settings, BadTomlFails) {
  auto s = parse("server = ");
  ASSERT_FALSE(s);
  EXPECT_EQ(s.error().code, ErrCode::validation);
  EXPECT_NE(s.error().detail.find("test.toml"), std::string::npos);
}

TEST(Settings, WrongTypeFails) {
  auto s = parse("server = 8443");
  ASSERT_FALSE(s);
  EXPECT_NE(s.error().detail.find("server: expected a string"), std::string::npos);
}

TEST(Settings, UnknownAndCommandLineOnlyKeysWarn) {
  auto s = parse(R"(
server = "https://a"
allow_http = true
password = "x"
colour = "blue"
)");
  ASSERT_TRUE(s);
  ASSERT_EQ(s->warnings.size(), 3U);
  std::string all;
  for (const auto& w : s->warnings) {
    all += w + "\n";
  }
  EXPECT_NE(all.find("allow_http: ignored"), std::string::npos);
  EXPECT_NE(all.find("password: ignored"), std::string::npos);
  EXPECT_NE(all.find("colour: unknown key"), std::string::npos);
}

TEST(Settings, OverlayHighWinsPerKey) {
  Settings low;
  low.server = "https://system";
  low.from.server = "/etc/ftc/ftc.toml";
  low.ca_file = fs::path{"/etc/ftc/ca.pem"};
  low.from.ca_file = "/etc/ftc/ftc.toml";

  Settings high;
  high.server = "https://env";
  high.from.server = "env FTC_SERVER";
  high.user = "bob";
  high.from.user = "--user";

  const auto s = overlay(low, high);
  EXPECT_EQ(s.server, "https://env");
  EXPECT_EQ(s.from.server, "env FTC_SERVER");
  EXPECT_EQ(s.user, "bob");
  EXPECT_EQ(s.from.user, "--user");
  EXPECT_EQ(s.ca_file, fs::path{"/etc/ftc/ca.pem"});  // not set in high: kept
  EXPECT_EQ(s.from.ca_file, "/etc/ftc/ftc.toml");
}

TEST(Settings, OverlayEmptyValueOverrides) {
  Settings low;
  low.ca_file = fs::path{"/etc/ftc/ca.pem"};
  Settings high;
  high.ca_file = fs::path{};
  const auto s = overlay(low, high);
  ASSERT_TRUE(s.ca_file);
  EXPECT_TRUE(s.ca_file->empty());
}

#ifndef _WIN32
class SettingsFiles : public ::testing::Test {
protected:
  void SetUp() override {
    root_ =
        fs::temp_directory_path()
        / std::format("ftc-settings-test-{}", ::testing::UnitTest::GetInstance()->random_seed());
    fs::remove_all(root_);
    fs::create_directories(root_);
    if (const char* v = std::getenv("XDG_CONFIG_HOME")) {  // NOLINT(concurrency-mt-unsafe)
      saved_xdg_ = v;
    }
    if (const char* v = std::getenv("HOME")) {  // NOLINT(concurrency-mt-unsafe)
      saved_home_ = v;
    }
  }
  void TearDown() override {
    restore("XDG_CONFIG_HOME", saved_xdg_);
    restore("HOME", saved_home_);
    fs::remove_all(root_);
  }
  static void restore(const char* name, const std::optional<std::string>& value) {
    if (value) {
      ::setenv(name, value->c_str(), 1);  // NOLINT(concurrency-mt-unsafe)
    } else {
      ::unsetenv(name);  // NOLINT(concurrency-mt-unsafe)
    }
  }

  fs::path root_;
  std::optional<std::string> saved_xdg_;
  std::optional<std::string> saved_home_;
};

TEST_F(SettingsFiles, DefaultFilesSystemThenXdg) {
  ::setenv("XDG_CONFIG_HOME", root_.c_str(), 1);  // NOLINT(concurrency-mt-unsafe)
  const auto files = default_settings_files();
  ASSERT_EQ(files.size(), 2U);
  EXPECT_EQ(files[0], fs::path{FTC_SYSTEM_CONFIG_FOR_TEST});
  EXPECT_EQ(files[1], root_ / "ftc" / "ftc.toml");
}

TEST_F(SettingsFiles, DefaultUserFileUnderHomeWithoutXdg) {
  ::unsetenv("XDG_CONFIG_HOME");       // NOLINT(concurrency-mt-unsafe)
  ::setenv("HOME", root_.c_str(), 1);  // NOLINT(concurrency-mt-unsafe)
  const auto files = default_settings_files();
  ASSERT_EQ(files.size(), 2U);
  EXPECT_EQ(files[1], root_ / ".config" / "ftc" / "ftc.toml");
}

TEST_F(SettingsFiles, RelativeXdgIsIgnored) {
  ::setenv("XDG_CONFIG_HOME", "relative/dir", 1);  // NOLINT(concurrency-mt-unsafe)
  ::setenv("HOME", root_.c_str(), 1);              // NOLINT(concurrency-mt-unsafe)
  const auto files = default_settings_files();
  ASSERT_EQ(files.size(), 2U);
  EXPECT_EQ(files[1], root_ / ".config" / "ftc" / "ftc.toml");
}

TEST_F(SettingsFiles, MissingFileIsNothing) {
  auto s = load_settings_file_if_exists(root_ / "nope.toml");
  ASSERT_TRUE(s);
  EXPECT_FALSE(*s);
  EXPECT_FALSE(load_settings_file(root_ / "nope.toml"));
}

TEST_F(SettingsFiles, LoadsFileAndResolvesAgainstIt) {
  const auto path = root_ / "ftc.toml";
  std::ofstream(path) << "server = \"https://x\"\nca_file = \"ca.pem\"\n";
  auto s = load_settings_file_if_exists(path);
  ASSERT_TRUE(s) << s.error().detail;
  ASSERT_TRUE(*s);
  EXPECT_EQ((*s)->values.server, "https://x");
  EXPECT_EQ((*s)->values.ca_file, root_ / "ca.pem");
  EXPECT_EQ((*s)->values.from.server, path.string());
}
#endif

}  // namespace
}  // namespace confide::client
