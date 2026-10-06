#pragma once
#include "transaction/transaction.hpp"

namespace confide::transaction {

// Pure rules of the state machine (Architecture §5), no I/O:
//
//   open      -> committed | expired
//   committed -> delivered | expired
//   delivered -> expired
//   expired   -> (deleted by the retention job)
//
// Staying in the same state is not a transition and is never allowed, so
// committing twice or delivering twice is a `conflict`.
bool can_transition(State from, State to);

// Like can_transition() but with the error the API returns:
//   `gone`     - `from` is expired
//   `conflict` - any other forbidden transition
Result<void> check_transition(State from, State to);

// Adding or deleting a file (FR-2, FR-3):
//   `conflict` - committed or delivered (immutable)
//   `gone`     - expired
Result<void> require_mutable(State state);

// Downloading a file or reading the manifest (FR-4, FR-12):
//   `conflict` - still open, content may change
//   `gone`     - expired
Result<void> require_readable(State state);

}  // namespace confide::transaction
