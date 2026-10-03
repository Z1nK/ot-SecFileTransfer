#include <transaction/codec.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

using confide::common::ErrCode;
using confide::common::parse_iso8601;
using confide::common::RelativePath;
using confide::common::Sha256Digest;
using confide::common::TransactionId;
using confide::transaction::decode_manifest;
using confide::transaction::decode_meta;
using confide::transaction::encode_manifest;
using confide::transaction::encode_meta;
using confide::transaction::FileEntry;
using confide::transaction::State;
using confide::transaction::Transaction;

namespace {

constexpr std::string_view kId = "123e4567-e89b-42d3-a456-426614174000";
const std::string kHexA(64, 'a');
const std::string kHexB(64, 'b');

FileEntry entry(std::string_view path, std::uint64_t size, const std::string& hex) {
  return {.path = RelativePath::parse(path).value(),
          .size = size,
          .sha256 = Sha256Digest::from_hex(hex).value()};
}

Transaction make_tx() {
  return {.id = TransactionId::parse(kId).value(),
          .sender = "alice",
          .target_user = "bob",
          .target_server = "siteB",
          .origin_server = "",
          .state = State::open,
          .created_at = parse_iso8601("2026-09-28T10:00:00Z").value(),
          .expires_at = parse_iso8601("2026-10-28T10:00:00Z").value(),
          .files = {entry("a.csv", 10, kHexA), entry("report/b.png", 20, kHexB)}};
}

// A valid meta.json with `field` replaced by `raw` (a JSON literal), or
// removed when `raw` is empty.
std::string meta_with(std::string_view field, std::string_view raw) {
  std::string out = R"({"v":1,)";
  const std::pair<std::string_view, std::string_view> fields[] = {
      {"id", R"("123e4567-e89b-42d3-a456-426614174000")"},
      {"sender", R"("alice")"},
      {"target_user", R"("bob")"},
      {"target_server", R"("")"},
      {"origin_server", R"("")"},
      {"state", R"("open")"},
      {"created_at", R"("2026-09-28T10:00:00Z")"},
      {"expires_at", R"("2026-10-28T10:00:00Z")"},
      {"files", "[]"},
  };
  for (const auto& [name, value] : fields) {
    const auto v = name == field ? raw : value;
    if (!v.empty()) {
      out += '"' + std::string{name} + "\":" + std::string{v} + ',';
    }
  }
  out.back() = '}';
  return out;
}

void expect_invalid_meta(std::string_view json) {
  const auto r = decode_meta(json);
  ASSERT_FALSE(r) << json;
  EXPECT_EQ(r.error().code, ErrCode::validation) << json;
}

}  // namespace

// --- meta.json -------------------------------------------------------------

TEST(CodecMeta, RoundTripOpen) {
  const auto tx = make_tx();
  const auto decoded = decode_meta(encode_meta(tx));
  ASSERT_TRUE(decoded) << decoded.error().detail;
  EXPECT_EQ(*decoded, tx);
}

TEST(CodecMeta, RoundTripWithOptionalTimes) {
  auto tx = make_tx();
  tx.state = State::delivered;
  tx.origin_server = "siteA";
  tx.committed_at = parse_iso8601("2026-09-28T10:05:00Z").value();
  tx.delivered_at = parse_iso8601("2026-09-28T10:06:00Z").value();

  const auto decoded = decode_meta(encode_meta(tx));
  ASSERT_TRUE(decoded) << decoded.error().detail;
  EXPECT_EQ(*decoded, tx);
}

TEST(CodecMeta, OmitsUnsetOptionalTimes) {
  const auto json = encode_meta(make_tx());
  EXPECT_EQ(json.find("committed_at"), std::string::npos);
  EXPECT_EQ(json.find("delivered_at"), std::string::npos);
}

TEST(CodecMeta, NullOptionalTimeIsUnset) {
  const auto r = decode_meta(meta_with("state", R"("open","committed_at":null)"));
  ASSERT_TRUE(r) << r.error().detail;
  EXPECT_FALSE(r->committed_at);
}

TEST(CodecMeta, TemplateIsValid) {
  // Guards the negative tests below: they must fail only because of the
  // one field they change.
  const auto r = decode_meta(meta_with("", ""));
  ASSERT_TRUE(r) << r.error().detail;
}

TEST(CodecMeta, RejectsMalformedJson) {
  expect_invalid_meta("");
  expect_invalid_meta("{");
  expect_invalid_meta("[]");
  expect_invalid_meta(R"("text")");
}

TEST(CodecMeta, RejectsWrongVersion) {
  auto json = meta_with("", "");
  json.replace(json.find(R"("v":1)"), 5, R"("v":2)");
  expect_invalid_meta(json);
}

TEST(CodecMeta, RejectsMissingFields) {
  for (const auto* field : {"id", "sender", "target_user", "target_server", "origin_server",
                            "state", "created_at", "expires_at", "files"}) {
    SCOPED_TRACE(field);
    expect_invalid_meta(meta_with(field, ""));
  }
}

TEST(CodecMeta, RejectsBadValues) {
  expect_invalid_meta(meta_with("id", R"("not-a-uuid")"));
  expect_invalid_meta(meta_with("sender", R"("")"));
  expect_invalid_meta(meta_with("target_user", "42"));
  expect_invalid_meta(meta_with("state", R"("closed")"));
  expect_invalid_meta(meta_with("created_at", R"("2026-09-28 10:00:00")"));
  expect_invalid_meta(meta_with("files", "{}"));
}

TEST(CodecMeta, ErrorNamesTheField) {
  const auto r = decode_meta(meta_with("created_at", R"("yesterday")"));
  ASSERT_FALSE(r);
  EXPECT_TRUE(r.error().detail.starts_with("created_at")) << r.error().detail;
}

// --- files (shared by both documents) --------------------------------------

TEST(CodecFiles, RejectsBadEntries) {
  const std::string good_hex = '"' + kHexA + '"';
  for (const auto& files : std::vector<std::string>{
           R"([1])",
           R"([{"size":1,"sha256":)" + good_hex + "}]",
           R"([{"path":"../x","size":1,"sha256":)" + good_hex + "}]",
           R"([{"path":"/abs","size":1,"sha256":)" + good_hex + "}]",
           R"([{"path":"a","size":-1,"sha256":)" + good_hex + "}]",
           R"([{"path":"a","size":1.5,"sha256":)" + good_hex + "}]",
           R"([{"path":"a","size":1,"sha256":"abc"}])",
       }) {
    SCOPED_TRACE(files);
    expect_invalid_meta(meta_with("files", files));
  }
}

TEST(CodecFiles, RejectsDuplicatePaths) {
  const std::string e = R"({"path":"a","size":1,"sha256":")" + kHexA + R"("})";
  expect_invalid_meta(meta_with("files", "[" + e + "," + e + "]"));
}

TEST(CodecFiles, SortsByPath) {
  const auto file = [](std::string_view path) {
    return R"({"path":")" + std::string{path} + R"(","size":1,"sha256":")" + kHexA + R"("})";
  };
  const auto r = decode_manifest(R"({"v":1,"files":[)" + file("b") + "," + file("a/z") + "," +
                                 file("a") + "]}");
  ASSERT_TRUE(r) << r.error().detail;
  ASSERT_EQ(r->size(), 3U);
  EXPECT_EQ((*r)[0].path.str(), "a");
  EXPECT_EQ((*r)[1].path.str(), "a/z");
  EXPECT_EQ((*r)[2].path.str(), "b");
}

TEST(CodecFiles, SizeAboveInt64) {
  auto tx = make_tx();
  tx.files = {entry("big", UINT64_MAX, kHexA)};
  const auto r = decode_manifest(encode_manifest(tx));
  ASSERT_TRUE(r) << r.error().detail;
  EXPECT_EQ((*r)[0].size, UINT64_MAX);
}

// --- manifest.json ---------------------------------------------------------

TEST(CodecManifest, ExactFormat) {
  // The manifest is also what a peer receives, so its layout is pinned.
  auto tx = make_tx();
  tx.files = {entry("a.csv", 10, kHexA)};
  EXPECT_EQ(encode_manifest(tx),
            R"({"v":1,"id":"123e4567-e89b-42d3-a456-426614174000","files":[)"
            R"({"path":"a.csv","size":10,"sha256":")" +
                kHexA + R"("}]})");
}

TEST(CodecManifest, RoundTrip) {
  const auto tx = make_tx();
  const auto r = decode_manifest(encode_manifest(tx));
  ASSERT_TRUE(r) << r.error().detail;
  EXPECT_EQ(*r, tx.files);
}

TEST(CodecManifest, AcceptsUppercaseHex) {
  const std::string upper(64, 'A');
  const auto r = decode_manifest(R"({"v":1,"files":[{"path":"a","size":1,"sha256":")" + upper +
                                 R"("}]})");
  ASSERT_TRUE(r) << r.error().detail;
  EXPECT_EQ((*r)[0].sha256.to_hex(), kHexA);
}

TEST(CodecManifest, RejectsMissingVersion) {
  const auto r = decode_manifest(R"({"files":[]})");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrCode::validation);
}
