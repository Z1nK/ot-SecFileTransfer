// ftc inbox, outbox, show, files: read-only views of transactions.
#include "commands.hpp"
#include <client/reply.hpp>
#include "session.hpp"

#include <boost/beast/http/verb.hpp>

#include <format>
#include <iostream>

namespace confide::cli {

namespace {

using http_verb = boost::beast::http::verb;

// What the forwarding error means for the sender. The server tells no more
// than these two; the cause of a forwarding error is in its logs.
std::string_view forward_reason(std::string_view error) {
  if (error == "unknown_destination") {
    return "no such user on that server";
  }
  return "forwarding error; ask your administrator if it stays";
}

// The forwarding object of an outgoing transaction, or nullptr.
const json::object* forwarding(const json::object& tx) {
  const auto* v = tx.if_contains("forwarding");
  return v != nullptr && v->is_object() ? &v->get_object() : nullptr;
}

// One line under the outbox for a transaction that did not get through.
void print_forward_note(const json::object& tx, const json::object& fwd) {
  const auto status = str(fwd, "status");
  const auto error = str(fwd, "error");
  if (error.empty()) {
    return;  // pending, nothing failed yet
  }
  const auto id = str(tx, "id");
  const auto attempts = num(fwd, "attempts");
  if (status == "failed") {
    std::cout << std::format("{}: not delivered: {}; once fixed: ftc retry {}\n", id,
                             forward_reason(error), id);
  } else if (status == "retrying") {
    std::cout << std::format("{}: {}; attempt {} failed, next at {}\n", id,
                             forward_reason(error), attempts, str(fwd, "next_attempt_at"));
  } else {
    std::cout << std::format("{}: retry asked for, next attempt at {}; last one: {}\n", id,
                             str(fwd, "next_attempt_at"), forward_reason(error));
  }
}

Result<void> list_box(const Global& global, std::string_view box) {
  CFD_TRY(client, connect(global));
  CFD_TRY(reply, client.call(http_verb::get, std::format("/transactions?box={}", box)));
  CFD_TRY(list, parse_list(reply, "transactions"));

  const bool in = box == "in";
  std::cout << std::format("{:<36}  {:<9}  {:<24}  {:>5}  {:>12}  {:<20}  {}\n", "ID", "STATE",
                           in ? "FROM" : "TO", "FILES", "BYTES", "CREATED", "COMMENT");
  for (const auto& v : list) {
    if (!v.is_object()) {
      continue;
    }
    const auto& tx = v.get_object();
    const auto peer =
        in ? address(tx, "sender", "origin_server") : address(tx, "target_user", "target_server");
    // A transaction on its way to another server shows how that goes
    // instead of `committed`: pending, retrying or failed.
    const auto* fwd = forwarding(tx);
    const auto state = fwd != nullptr ? str(*fwd, "status") : str(tx, "state");
    std::cout << std::format("{:<36}  {:<9}  {:<24}  {:>5}  {:>12}  {:<20}  {}\n", str(tx, "id"),
                             state, peer, num(tx, "file_count"), num(tx, "total_bytes"),
                             str(tx, "created_at"), str(tx, "comment"));
  }
  if (!in) {
    for (const auto& v : list) {
      if (const auto* tx = v.if_object(); tx != nullptr) {
        if (const auto* fwd = forwarding(*tx); fwd != nullptr) {
          print_forward_note(*tx, *fwd);
        }
      }
    }
  }
  return {};
}

void id_option(po::options_description& /*desc*/, po::options_description& hidden,
               po::positional_options_description& pos) {
  hidden.add_options()("id", po::value<std::string>()->required());
  pos.add("id", 1);
}

Result<void> run_show(const Global& global, const po::variables_map& vm) {
  CFD_TRY(id, common::TransactionId::parse(vm["id"].as<std::string>()));
  CFD_TRY(client, connect(global));
  CFD_TRY(reply, client.call(http_verb::get, tx_target(id)));
  CFD_TRY(tx, parse_object(reply));
  const auto text = [](const json::value& value) {
    return value.is_string() ? std::string{value.get_string()} : json::serialize(value);
  };
  for (const auto& [key, value] : tx) {
    if (value.is_object()) {  // "forwarding": one line per field
      for (const auto& [sub, inner] : value.get_object()) {
        std::cout << std::format("{:<14} {}\n", std::format("{}.{}", std::string_view{key},
                                                            std::string_view{sub}),
                                 text(inner));
      }
      continue;
    }
    std::cout << std::format("{:<14} {}\n", std::string_view{key}, text(value));
  }
  return {};
}

Result<void> run_files(const Global& global, const po::variables_map& vm) {
  CFD_TRY(id, common::TransactionId::parse(vm["id"].as<std::string>()));
  CFD_TRY(client, connect(global));
  CFD_TRY(reply, client.call(http_verb::get, tx_target(id) + "/files"));
  CFD_TRY(list, parse_list(reply, "files"));
  for (const auto& v : list) {
    if (v.is_object()) {
      const auto& f = v.get_object();
      std::cout << std::format("{:>12}  {}  {}\n", num(f, "size"), str(f, "sha256"),
                               str(f, "path"));
    }
  }
  return {};
}

}  // namespace

const Command kInbox{
    .name = "inbox",
    .synopsis = "",
    .run = [](const Global& g, const po::variables_map&) { return list_box(g, "in"); },
};
const Command kOutbox{
    .name = "outbox",
    .synopsis = "",
    .run = [](const Global& g, const po::variables_map&) { return list_box(g, "out"); },
};
const Command kShow{.name = "show", .synopsis = " ID", .options = id_option, .run = run_show};
const Command kFiles{.name = "files", .synopsis = " ID", .options = id_option, .run = run_files};

}  // namespace confide::cli
