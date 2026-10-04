#include <api/json.hpp>

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <string>

using confide::common::ErrCode;
using confide::common::RelativePath;
using confide::common::Sha256;
using confide::common::Timestamp;
using confide::common::TransactionId;
using confide::transaction::FileEntry;
using confide::transaction::ReceiveRequest;
using confide::transaction::State;
using confide::transaction::Transaction;

namespace api = confide::api;
namespace json = boost::json;

namespace {

Timestamp ts(std::string_view s) {
  return confide::common::parse_iso8601(s).value();
}

FileEntry entry(std::string_view path, std::string_view content) {
  return FileEntry{
      .path = RelativePath::parse(path).value(),
      .size = content.size(),
      .sha256 = Sha256::of(content).value(),
  };
}

Transaction sample() {
  return Transaction{
      .id = TransactionId::generate(),
      .sender = "alice",
      .target_user = "bob",
      .target_server = "siteB",
      .origin_server = "",
      .state = State::committed,
      .created_at = ts("2026-10-01T10:00:00Z"),
      .expires_at = ts("2026-10-31T10:00:00Z"),
      .committed_at = ts("2026-10-01T10:05:00Z"),
      .files = {entry("report/a.csv", "abc"), entry("report/b.png", "hello")},
  };
}

json::object parse(const std::string& s) {
  return json::parse(s).as_object();
}

}  // namespace

TEST(DecodeCreate, FullBody) {
  auto r = api::decode_create(R"({"target_user":"bob","target_server":"siteB","retention_days":7})");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->target_user, "bob");
  EXPECT_EQ(r->target_server, "siteB");
  EXPECT_EQ(r->retention_days, 7);
}

TEST(DecodeCreate, OptionalFieldsDefault) {
  auto r = api::decode_create(R"({"target_user":"bob","target_server":null})");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->target_server, "");
  EXPECT_EQ(r->retention_days, 0);
}

TEST(DecodeCreate, RejectsBadInput) {
  EXPECT_EQ(api::decode_create("not json").error().code, ErrCode::validation);
  EXPECT_EQ(api::decode_create("[]").error().code, ErrCode::validation);
  EXPECT_EQ(api::decode_create("{}").error().code, ErrCode::validation);
  EXPECT_EQ(api::decode_create(R"({"target_user":""})").error().code, ErrCode::validation);
  EXPECT_EQ(api::decode_create(R"({"target_user":1})").error().code, ErrCode::validation);
  EXPECT_EQ(api::decode_create(R"({"target_user":"bob","target_server":5})").error().code,
            ErrCode::validation);
  EXPECT_EQ(api::decode_create(R"({"target_user":"bob","retention_days":1.5})").error().code,
            ErrCode::validation);
  EXPECT_EQ(api::decode_create(R"({"target_user":"bob","retention_days":"7"})").error().code,
            ErrCode::validation);
  EXPECT_EQ(
      api::decode_create(R"({"target_user":"bob","retention_days":99999999999})").error().code,
      ErrCode::validation);
}

TEST(Receive, RoundTrip) {
  const ReceiveRequest req{
      .id = TransactionId::generate(),
      .sender = "alice",
      .target_user = "bob",
      .created_at = ts("2026-10-01T10:00:00Z"),
      .expires_at = ts("2026-10-31T10:00:00Z"),
  };
  auto back = api::decode_receive(api::encode_receive(req));
  ASSERT_TRUE(back);
  EXPECT_EQ(back->id, req.id);
  EXPECT_EQ(back->sender, req.sender);
  EXPECT_EQ(back->target_user, req.target_user);
  EXPECT_EQ(back->created_at, req.created_at);
  EXPECT_EQ(back->expires_at, req.expires_at);
}

TEST(Receive, RejectsBadFields) {
  const std::string ok_id = TransactionId::generate().str();
  const auto body = [&](std::string_view id, std::string_view created) {
    return std::string{R"({"id":")"} + std::string{id} +
           R"(","sender":"alice","target_user":"bob","created_at":")" + std::string{created} +
           R"(","expires_at":"2026-10-31T10:00:00Z"})";
  };
  ASSERT_TRUE(api::decode_receive(body(ok_id, "2026-10-01T10:00:00Z")));
  EXPECT_EQ(api::decode_receive(body("not-an-id", "2026-10-01T10:00:00Z")).error().code,
            ErrCode::validation);
  EXPECT_EQ(api::decode_receive(body(ok_id, "yesterday")).error().code, ErrCode::validation);
  EXPECT_EQ(api::decode_receive(R"({"id":"x"})").error().code, ErrCode::validation);
}

TEST(EncodeTransaction, Fields) {
  const auto tx = sample();
  const auto obj = parse(api::encode_transaction(tx));
  EXPECT_EQ(obj.at("id").as_string(), tx.id.str());
  EXPECT_EQ(obj.at("state").as_string(), "committed");
  EXPECT_EQ(obj.at("sender").as_string(), "alice");
  EXPECT_EQ(obj.at("target_user").as_string(), "bob");
  EXPECT_EQ(obj.at("target_server").as_string(), "siteB");
  EXPECT_EQ(obj.at("created_at").as_string(), "2026-10-01T10:00:00Z");
  EXPECT_EQ(obj.at("expires_at").as_string(), "2026-10-31T10:00:00Z");
  EXPECT_EQ(obj.at("committed_at").as_string(), "2026-10-01T10:05:00Z");
  EXPECT_FALSE(obj.contains("delivered_at"));
  EXPECT_EQ(obj.at("file_count").to_number<int>(), 2);
  EXPECT_EQ(obj.at("total_bytes").to_number<int>(), 8);
  EXPECT_FALSE(obj.contains("files"));
}

TEST(EncodeTransaction, OpenHasNoCommitTime) {
  auto tx = sample();
  tx.state = State::open;
  tx.committed_at.reset();
  const auto obj = parse(api::encode_transaction(tx));
  EXPECT_EQ(obj.at("state").as_string(), "open");
  EXPECT_FALSE(obj.contains("committed_at"));
}

TEST(EncodeTransactionList, WrapsItems) {
  const auto obj = parse(api::encode_transaction_list({sample(), sample()}));
  ASSERT_EQ(obj.at("transactions").as_array().size(), 2U);
  EXPECT_EQ(parse(api::encode_transaction_list({})).at("transactions").as_array().size(), 0U);
}

TEST(EncodeFiles, ListAndSingle) {
  const auto tx = sample();
  const auto obj = parse(api::encode_files(tx.files));
  const auto& files = obj.at("files").as_array();
  ASSERT_EQ(files.size(), 2U);
  EXPECT_EQ(files[0].at("path").as_string(), "report/a.csv");
  EXPECT_EQ(files[0].at("size").to_number<int>(), 3);
  EXPECT_EQ(files[0].at("sha256").as_string(), tx.files[0].sha256.to_hex());

  const auto one = parse(api::encode_file(tx.files[1]));
  EXPECT_EQ(one.at("path").as_string(), "report/b.png");
  EXPECT_EQ(one.at("size").to_number<int>(), 5);
}
