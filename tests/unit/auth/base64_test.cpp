#include <auth/base64.hpp>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

using confide::auth::base64_decode;
using confide::auth::base64_encode;
using confide::common::ByteSpan;
using confide::common::ErrCode;

namespace {

std::string encode(std::string_view text) {
  return base64_encode(ByteSpan(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

std::string decode(std::string_view text) {
  auto bytes = base64_decode(text);
  EXPECT_TRUE(bytes.has_value()) << text;
  return bytes ? std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size()) : "";
}

}  // namespace

// RFC 4648 §10 test vectors.
TEST(Base64, Rfc4648Vectors) {
  const std::pair<std::string_view, std::string_view> vectors[] = {
      {"", ""},
      {"f", "Zg=="},
      {"fo", "Zm8="},
      {"foo", "Zm9v"},
      {"foob", "Zm9vYg=="},
      {"fooba", "Zm9vYmE="},
      {"foobar", "Zm9vYmFy"},
  };
  for (const auto& [plain, coded] : vectors) {
    EXPECT_EQ(encode(plain), coded);
    EXPECT_EQ(decode(coded), plain);
  }
}

TEST(Base64, BasicCredentials) {
  EXPECT_EQ(decode("YWxpY2U6czNjcjM6dA=="), "alice:s3cr3:t");
}

TEST(Base64, RejectsMalformed) {
  for (std::string_view bad : {"Zg=", "Zg", "Z===", "=Zg=", "Zg==Zm8=", "Zm9v\n", "Zm 9", "Zm9*"}) {
    auto r = base64_decode(bad);
    ASSERT_FALSE(r.has_value()) << bad;
    EXPECT_EQ(r.error().code, ErrCode::validation);
  }
}
