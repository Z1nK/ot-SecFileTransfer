#include <transaction/transaction.hpp>

#include <gtest/gtest.h>

#include <string_view>

using confide::common::ErrCode;
using confide::common::RelativePath;
using confide::common::Sha256Digest;
using confide::common::TransactionId;
using confide::transaction::FileEntry;
using confide::transaction::parse_state;
using confide::transaction::State;
using confide::transaction::Transaction;

namespace {

FileEntry entry(std::string_view path, std::uint64_t size) {
  return {.path = RelativePath::parse(path).value(),
          .size = size,
          .sha256 = Sha256Digest{Sha256Digest::Bytes{}}};
}

Transaction make_tx() {
  return {.id = TransactionId::generate(),
          .sender = "alice",
          .target_user = "bob",
          .target_server = "",
          .origin_server = "",
          .state = State::open,
          .created_at = {},
          .expires_at = {}};
}

}  // namespace

TEST(State, RoundTripsThroughText) {
  for (const auto s : {State::open, State::committed, State::delivered, State::expired}) {
    const auto parsed = parse_state(to_string(s));
    ASSERT_TRUE(parsed) << to_string(s);
    EXPECT_EQ(*parsed, s);
  }
}

TEST(State, RejectsUnknownText) {
  for (const auto text : {"", "Open", "OPEN", "open ", "closed"}) {
    const auto r = parse_state(text);
    ASSERT_FALSE(r) << '"' << text << '"';
    EXPECT_EQ(r.error().code, ErrCode::validation);
  }
}

TEST(TransactionModel, FindFileInSortedList) {
  auto tx = make_tx();
  tx.files = {entry("a.csv", 1), entry("report/b.png", 2), entry("report/c.txt", 3)};

  const auto* found = tx.find_file(RelativePath::parse("report/b.png").value());
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->size, 2U);

  EXPECT_EQ(tx.find_file(RelativePath::parse("report").value()), nullptr);
  EXPECT_EQ(tx.find_file(RelativePath::parse("z.txt").value()), nullptr);
}

TEST(TransactionModel, TotalBytes) {
  auto tx = make_tx();
  EXPECT_EQ(tx.total_bytes(), 0U);

  tx.files = {entry("a", 10), entry("b", 20), entry("c", 12)};
  EXPECT_EQ(tx.total_bytes(), 42U);
}
