#include <logging/tech_log.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using confide::logging::FileSink;
using confide::logging::LogLevel;

namespace fs = std::filesystem;

namespace {

std::string read_all(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Fresh directory per test, removed afterwards.
class FileSinkTest : public ::testing::Test {
protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::temp_directory_path() / (std::string{"confide_"} + info->name());
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    log_ = dir_ / "tech.log";
    rotated_ = dir_ / "tech.log.1";
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  fs::path dir_;
  fs::path log_;
  fs::path rotated_;
};

}  // namespace

TEST_F(FileSinkTest, CreatesFileAndAppendsLines) {
  FileSink sink(log_, /*rotate_bytes=*/0);
  sink.write(LogLevel::info, "one");
  sink.write(LogLevel::error, "two");

  EXPECT_EQ(read_all(log_), "one\ntwo\n");
}

TEST_F(FileSinkTest, AppendsToExistingFile) {
  {
    std::ofstream(log_) << "old\n";
  }
  FileSink sink(log_, 0);
  sink.write(LogLevel::info, "new");

  EXPECT_EQ(read_all(log_), "old\nnew\n");
}

TEST_F(FileSinkTest, ZeroRotateBytesNeverRotates) {
  FileSink sink(log_, 0);
  for (int i = 0; i < 100; ++i) {
    sink.write(LogLevel::info, "0123456789");
  }
  EXPECT_FALSE(fs::exists(rotated_));
  EXPECT_EQ(fs::file_size(log_), 100U * 11U);
}

TEST_F(FileSinkTest, RotatesWhenLimitWouldBeExceeded) {
  FileSink sink(log_, /*rotate_bytes=*/10);
  sink.write(LogLevel::info, "aaaa");  // 5 bytes
  sink.write(LogLevel::info, "bbbb");  // 10 bytes, still within the limit
  EXPECT_FALSE(fs::exists(rotated_));

  sink.write(LogLevel::info, "cccc");  // would exceed 10 -> rotate first
  EXPECT_EQ(read_all(rotated_), "aaaa\nbbbb\n");
  EXPECT_EQ(read_all(log_), "cccc\n");
}

TEST_F(FileSinkTest, SecondRotationReplacesOldBackup) {
  FileSink sink(log_, 5);
  sink.write(LogLevel::info, "aaaa");
  sink.write(LogLevel::info, "bbbb");
  sink.write(LogLevel::info, "cccc");

  EXPECT_EQ(read_all(rotated_), "bbbb\n");
  EXPECT_EQ(read_all(log_), "cccc\n");
}

TEST_F(FileSinkTest, CountsExistingSizeTowardsRotation) {
  {
    std::ofstream(log_) << "123456789\n";  // 10 bytes already on disk
  }
  FileSink sink(log_, 10);
  sink.write(LogLevel::info, "x");  // 10 + 1 > 10 -> rotate

  EXPECT_EQ(read_all(rotated_), "123456789\n");
  EXPECT_EQ(read_all(log_), "x\n");
}
