#include <transaction/state_machine.hpp>

#include <gtest/gtest.h>

#include <array>
#include <utility>

using confide::common::ErrCode;
using confide::transaction::can_transition;
using confide::transaction::check_transition;
using confide::transaction::require_mutable;
using confide::transaction::require_readable;
using confide::transaction::State;

namespace {

constexpr std::array kAllStates{State::open, State::committed, State::delivered, State::expired};

// Every edge of the diagram in Architecture §5; anything else is forbidden.
constexpr std::array<std::pair<State, State>, 5> kAllowed{{
    {State::open, State::committed},
    {State::open, State::expired},
    {State::committed, State::delivered},
    {State::committed, State::expired},
    {State::delivered, State::expired},
}};

bool is_allowed(State from, State to) {
  for (const auto& [f, t] : kAllowed) {
    if (f == from && t == to) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(StateMachine, MatchesDiagramForEveryPair) {
  for (const auto from : kAllStates) {
    for (const auto to : kAllStates) {
      EXPECT_EQ(can_transition(from, to), is_allowed(from, to))
          << to_string(from) << " -> " << to_string(to);
    }
  }
}

TEST(StateMachine, CheckTransitionAcceptsAllowedEdges) {
  for (const auto& [from, to] : kAllowed) {
    EXPECT_TRUE(check_transition(from, to)) << to_string(from) << " -> " << to_string(to);
  }
}

TEST(StateMachine, CommitTwiceIsConflict) {
  const auto r = check_transition(State::committed, State::committed);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrCode::conflict);
}

TEST(StateMachine, OpenCannotSkipToDelivered) {
  const auto r = check_transition(State::open, State::delivered);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrCode::conflict);
}

TEST(StateMachine, CannotGoBackToOpen) {
  for (const auto from : {State::committed, State::delivered}) {
    EXPECT_FALSE(can_transition(from, State::open)) << to_string(from);
  }
}

TEST(StateMachine, ExpiredIsFinalAndGone) {
  for (const auto to : kAllStates) {
    const auto r = check_transition(State::expired, to);
    ASSERT_FALSE(r) << to_string(to);
    EXPECT_EQ(r.error().code, ErrCode::gone);
  }
}

TEST(StateMachine, OnlyOpenIsMutable) {
  EXPECT_TRUE(require_mutable(State::open));

  for (const auto s : {State::committed, State::delivered}) {
    const auto r = require_mutable(s);
    ASSERT_FALSE(r) << to_string(s);
    EXPECT_EQ(r.error().code, ErrCode::conflict);
  }

  const auto r = require_mutable(State::expired);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrCode::gone);
}

TEST(StateMachine, ReadableOnlyAfterCommit) {
  EXPECT_TRUE(require_readable(State::committed));
  EXPECT_TRUE(require_readable(State::delivered));

  const auto open = require_readable(State::open);
  ASSERT_FALSE(open);
  EXPECT_EQ(open.error().code, ErrCode::conflict);

  const auto expired = require_readable(State::expired);
  ASSERT_FALSE(expired);
  EXPECT_EQ(expired.error().code, ErrCode::gone);
}
