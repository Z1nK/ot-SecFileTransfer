#include <common/error.hpp>

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <string_view>

using confide::common::ErrCode;
using confide::common::Error;
using confide::common::Result;

namespace {

constexpr ErrCode kAllCodes[] = {
    ErrCode::validation, ErrCode::auth,      ErrCode::forbidden,
    ErrCode::notfound,   ErrCode::conflict,  ErrCode::gone,
    ErrCode::integrity,  ErrCode::unknown_destination,
    ErrCode::unavailable, ErrCode::io,       ErrCode::internal,
};

Result<int> ok_int(int v) {
  return v;
}

Result<int> fail_int(ErrCode code) {
  return std::unexpected(Error::make(code, "leaf failed"));
}

Result<void> ok_void() {
  return {};
}

Result<void> fail_void() {
  return std::unexpected(Error::make(ErrCode::io, "void failed"));
}

// Adds 1 to the unwrapped value; used to check CFD_TRY binds and propagates.
Result<int> plus_one(Result<int> in) {
  CFD_TRY(v, std::move(in));
  return v + 1;
}

// Returns 7 unless CFD_TRYV propagated an error first.
Result<int> after_void(Result<void> in) {
  CFD_TRYV(std::move(in));
  return 7;
}

}  // namespace

TEST(ErrCodeToString, KnownNames) {
  EXPECT_EQ(to_string(ErrCode::validation), "validation");
  EXPECT_EQ(to_string(ErrCode::notfound), "notfound");
  EXPECT_EQ(to_string(ErrCode::unknown_destination), "unknown_destination");
  EXPECT_EQ(to_string(ErrCode::internal), "internal");
}

TEST(ErrCodeToString, EveryCodeHasUniqueLowercaseName) {
  std::set<std::string_view> names;
  for (ErrCode code : kAllCodes) {
    std::string_view name = to_string(code);
    EXPECT_NE(name, "unknown") << static_cast<int>(code);
    for (char c : name) {
      EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '_') << name;
    }
    EXPECT_TRUE(names.insert(name).second) << "duplicate name " << name;
  }
}

TEST(ErrorMake, StoresCodeDetailAndCallerLocation) {
  const auto line = std::source_location::current().line() + 1;
  Error e = Error::make(ErrCode::conflict, "already committed");

  EXPECT_EQ(e.code, ErrCode::conflict);
  EXPECT_EQ(e.detail, "already committed");
  EXPECT_EQ(e.origin.line(), line);
  EXPECT_NE(std::string_view{e.origin.file_name()}.find("error_test.cpp"),
            std::string_view::npos);
}

TEST(CfdTry, BindsValueOnSuccess) {
  auto r = plus_one(ok_int(41));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 42);
}

TEST(CfdTry, PropagatesErrorUnchanged) {
  auto r = plus_one(fail_int(ErrCode::gone));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrCode::gone);
  EXPECT_EQ(r.error().detail, "leaf failed");
}

TEST(CfdTryv, ContinuesOnSuccess) {
  auto r = after_void(ok_void());
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 7);
}

TEST(CfdTryv, PropagatesError) {
  auto r = after_void(fail_void());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrCode::io);
  EXPECT_EQ(r.error().detail, "void failed");
}
