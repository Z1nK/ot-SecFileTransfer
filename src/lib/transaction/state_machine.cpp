#include "transaction/state_machine.hpp"

#include <string>

namespace confide::transaction {

namespace {

using common::ErrCode;
using common::Error;

std::unexpected<Error> fail(ErrCode code, std::string detail) {
  return std::unexpected(Error::make(code, std::move(detail)));
}

}  // namespace

bool can_transition(State from, State to) {
  switch (from) {
  case State::open:
    return to == State::committed || to == State::expired;
  case State::committed:
    return to == State::delivered || to == State::expired;
  case State::delivered:
    return to == State::expired;
  case State::expired:
    return false;
  }
  return false;
}

Result<void> check_transition(State from, State to) {
  if (can_transition(from, to)) {
    return {};
  }
  const auto code = from == State::expired ? ErrCode::gone : ErrCode::conflict;
  return fail(code, "transaction is " + std::string{to_string(from)} + ", cannot become " +
                        std::string{to_string(to)});
}

Result<void> require_mutable(State state) {
  switch (state) {
  case State::open:
    return {};
  case State::committed:
  case State::delivered:
    return fail(ErrCode::conflict,
                "transaction is " + std::string{to_string(state)} + " and cannot be changed");
  case State::expired:
    return fail(ErrCode::gone, "transaction has expired");
  }
  return fail(ErrCode::internal, "bad state");
}

Result<void> require_readable(State state) {
  switch (state) {
  case State::committed:
  case State::delivered:
    return {};
  case State::open:
    return fail(ErrCode::conflict, "transaction is not committed yet");
  case State::expired:
    return fail(ErrCode::gone, "transaction has expired");
  }
  return fail(ErrCode::internal, "bad state");
}

}  // namespace confide::transaction
