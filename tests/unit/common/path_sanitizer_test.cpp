#include <common/path_sanitizer.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>

using confide::common::ErrCode;
using confide::common::kMaxComponentBytes;
using confide::common::kMaxPathBytes;
using confide::common::RelativePath;

namespace fs = std::filesystem;

class RelativePathParseAccepts : public ::testing::TestWithParam<std::string_view> {};

TEST_P(RelativePathParseAccepts, KeepsTextUnchanged) {
  auto p = RelativePath::parse(GetParam());
  ASSERT_TRUE(p.has_value()) << GetParam();
  EXPECT_EQ(p->str(), GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    Valid, RelativePathParseAccepts,
    ::testing::Values("a.csv",
                      "report/a.csv",
                      "report/img/b.png",
                      "..foo",             // dots inside a name are fine
                      "a..b/c...",
                      ".hidden/.env",
                      "with space/%41.txt",  // literal '%': no second URL-decode
                      "\xd0\xbe\xd1\x82\xd1\x87\xd1\x91\xd1\x82.pdf"));  // UTF-8 name

class RelativePathParseRejects : public ::testing::TestWithParam<std::string_view> {};

TEST_P(RelativePathParseRejects, WithValidationError) {
  auto p = RelativePath::parse(GetParam());
  ASSERT_FALSE(p.has_value()) << GetParam();
  EXPECT_EQ(p.error().code, ErrCode::validation);
}

INSTANTIATE_TEST_SUITE_P(
    Invalid, RelativePathParseRejects,
    ::testing::Values("",
                      "/etc/passwd",           // absolute
                      "..",                    // parent
                      "../a",
                      "a/../../b",
                      "a/..",
                      ".",
                      "./a",
                      "a/./b",
                      "a//b",                  // empty component
                      "a/",                    // trailing slash
                      "/",
                      "..\\..\\x",             // Windows separator
                      "a\\b",
                      "C:/Windows",            // drive
                      "C:x",
                      "file.txt:stream",       // alternate data stream
                      "a\nb",                  // control characters
                      "a\tb",
                      "a\x7f",
                      std::string_view{"a\0b", 3}));  // NUL byte

TEST(RelativePathParse, LengthLimits) {
  const std::string max_part(kMaxComponentBytes, 'x');
  EXPECT_TRUE(RelativePath::parse(max_part).has_value());
  EXPECT_FALSE(RelativePath::parse(max_part + "x").has_value());

  std::string max_path;
  while (max_path.size() + 2 <= kMaxPathBytes) {
    max_path += max_path.empty() ? "d" : "/d";
  }
  max_path.resize(kMaxPathBytes, 'd');
  EXPECT_TRUE(RelativePath::parse(max_path).has_value());
  EXPECT_FALSE(RelativePath::parse(max_path + "d").has_value());
}

TEST(RelativePathFromLocal, BuildsSlashSeparatedPath) {
  auto p = RelativePath::from_local("/home/u/data", "/home/u/data/report/img/b.png");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->str(), "report/img/b.png");
}

TEST(RelativePathFromLocal, NormalisesBaseAndFile) {
  auto p = RelativePath::from_local("/home/u/data/", "/home/u/data/./report/a.csv");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->str(), "report/a.csv");
}

TEST(RelativePathFromLocal, RejectsFileOutsideBase) {
  EXPECT_FALSE(RelativePath::from_local("/home/u/data", "/home/u/other/a.csv").has_value());
  EXPECT_FALSE(RelativePath::from_local("/home/u/data", "/home/u/data/../x").has_value());
  EXPECT_FALSE(RelativePath::from_local("/home/u/data", "/home/u/data").has_value());
}

TEST(RelativePathFromLocal, RejectsNameThatFailsParse) {
  auto p = RelativePath::from_local("/base", "/base/12:00.log");
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error().code, ErrCode::validation);
}

TEST(RelativePathUnder, StaysInsideRoot) {
  auto p = RelativePath::parse("report/img/b.png");
  ASSERT_TRUE(p.has_value());
  const fs::path root = "/srv/storage/transactions/x/files";
  const fs::path full = p->under(root);

  EXPECT_EQ(full, root / "report" / "img" / "b.png");
  auto rel = full.lexically_relative(root);
  EXPECT_FALSE(rel.empty());
  EXPECT_NE(*rel.begin(), "..");
}

TEST(RelativePath, CompareAndHash) {
  auto a = RelativePath::parse("a/b");
  auto b = RelativePath::parse("a/c");
  ASSERT_TRUE(a && b);
  EXPECT_LT(*a, *b);

  std::unordered_set<RelativePath> set{*a, *b, *a};
  EXPECT_EQ(set.size(), 2U);
}
