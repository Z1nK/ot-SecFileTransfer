#pragma once
// Internal to TransactionService: only service.cpp and the service/*.cpp
// files include this. Error helpers and access rules shared by the sections.
#include "transaction/service.hpp"

#include <format>
#include <string>
#include <utility>

namespace confide::transaction::detail {

using common::ErrCode;
using common::Error;
using logging::BizEvent;
using logging::BizEventType;

inline std::unexpected<Error> fail(ErrCode code, std::string detail) {
  return std::unexpected(Error::make(code, std::move(detail)));
}

inline std::unexpected<Error> not_found(const common::TransactionId& id) {
  return fail(ErrCode::notfound, std::format("no transaction {}", id.str()));
}

inline std::unexpected<Error> forbidden(const common::TransactionId& id) {
  return fail(ErrCode::forbidden, std::format("no access to transaction {}", id.str()));
}

// "alice", or "peer:<server>" for a peer, as the business log expects.
inline std::string actor_of(const Caller& caller) {
  return caller.is_peer ? "peer:" + caller.name : caller.name;
}

inline BizEvent event_for(BizEventType type, const Transaction& tx, const Caller& caller) {
  return BizEvent{
      .type = type,
      .tx = tx.id,
      .actor = actor_of(caller),
      .from_user = tx.sender,
      .to_user = tx.target_user,
      .to_server = tx.target_server,
  };
}

// The user who created the transaction on this server.
inline bool is_sender(const Caller& caller, const Transaction& tx) {
  return !caller.is_peer && tx.origin_server.empty() && caller.name == tx.sender;
}

// The user the transaction is addressed to on this server, or a member of
// the group it is addressed to.
inline bool is_target(const Caller& caller, const Transaction& tx, const MembershipCheck& is_recipient) {
  return !caller.is_peer && tx.target_server.empty()
         && (caller.name == tx.target_user
             || (is_recipient && is_recipient(caller.name, tx.target_user)));
}

// The peer that pushed the transaction here.
inline bool is_origin_peer(const Caller& caller, const Transaction& tx) {
  return caller.is_peer && !tx.origin_server.empty() && caller.name == tx.origin_server;
}

// Who may add or delete files and commit: the sender, or the peer that pushed it.
inline bool may_write(const Caller& caller, const Transaction& tx) {
  return is_sender(caller, tx) || is_origin_peer(caller, tx);
}

// Who may see metadata and files: the sender always, the target once the
// sender has committed, the pushing peer.
inline bool may_read(const Caller& caller, const Transaction& tx, const MembershipCheck& is_recipient) {
  return is_sender(caller, tx) || is_origin_peer(caller, tx)
         || (is_target(caller, tx, is_recipient) && tx.state != State::open);
}

// Created here, addressed to another server, committed but not yet pushed.
inline bool is_outgoing(const Transaction& tx) {
  return tx.origin_server.empty() && !tx.target_server.empty() && tx.state == State::committed;
}

}  // namespace confide::transaction::detail
