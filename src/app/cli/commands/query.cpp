// ftc inbox, outbox, show, files: read-only views of transactions.
#include "commands.hpp"
#include "reply.hpp"
#include "session.hpp"

#include <boost/beast/http/verb.hpp>

#include <format>
#include <iostream>

namespace confide::cli {

namespace {

using http_verb = boost::beast::http::verb;

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
        in ? std::string{str(tx, "sender")} : address(tx, "target_user", "target_server");
    std::cout << std::format("{:<36}  {:<9}  {:<24}  {:>5}  {:>12}  {:<20}  {}\n", str(tx, "id"),
                             str(tx, "state"), peer, num(tx, "file_count"), num(tx, "total_bytes"),
                             str(tx, "created_at"), str(tx, "comment"));
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
  for (const auto& [key, value] : tx) {
    std::cout << std::format(
        "{:<14} {}\n", std::string_view{key},
        value.is_string() ? std::string{value.get_string()} : json::serialize(value));
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
