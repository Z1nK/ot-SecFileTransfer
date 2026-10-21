// ftc send: create a transaction, upload, commit.
#include "commands.hpp"
#include "local_files.hpp"
#include "reply.hpp"
#include "session.hpp"

#include <boost/beast/http/verb.hpp>

#include <iostream>
#include <vector>

namespace confide::cli {

namespace {

using http_verb = boost::beast::http::verb;

void send_options(po::options_description& desc, po::options_description& hidden,
                  po::positional_options_description& pos) {
  desc.add_options()("to,t",
                     po::value<std::string>()->required()->value_name("USER|GROUP[@SERVER]"),
                     "receiver, a user or a group; without @SERVER one on the same server")(
      "retention,r", po::value<int>()->value_name("DAYS"),
      "days to keep the transaction (server default if not set)")(
      "comment,m", po::value<std::string>()->value_name("TEXT"),
      "short note shown in inbox/outbox (at most 256 characters)");
  hidden.add_options()("path", po::value<std::vector<std::string>>()->required());
  pos.add("path", -1);
}

Result<void> run_send(const Global& global, const po::variables_map& vm) {
  const auto to = vm["to"].as<std::string>();
  const auto at = to.rfind('@');
  json::object body{{"target_user", to.substr(0, at)}};
  if (at != std::string::npos) {
    body["target_server"] = to.substr(at + 1);
  }
  if (vm.contains("retention")) {
    body["retention_days"] = vm["retention"].as<int>();
  }
  if (vm.contains("comment")) {
    body["comment"] = vm["comment"].as<std::string>();
  }

  CFD_TRY(files, collect_files(vm["path"].as<std::vector<std::string>>()));
  CFD_TRY(client, connect(global));

  CFD_TRY(created, client.call(http_verb::post, "/transactions", json::serialize(body)));
  CFD_TRY(tx, parse_object(created));
  CFD_TRY(id, common::TransactionId::parse(str(tx, "id")));
  std::cout << "created " << id.str() << " for " << to << '\n';

  // The transaction stays open on failure; say which one it is.
  auto upload_all = [&]() -> Result<void> {
    for (const auto& file : files) {
      CFD_TRY(sha, hash_file(file.source));
      CFD_TRYV(client.upload(tx_target(id) + "/files/" + net::url_encode_path(file.path.str()),
                             file.source, sha));
      std::cout << "  " << file.path.str() << '\n';
    }
    CFD_TRYV(client.call(http_verb::post, tx_target(id) + "/commit"));
    return {};
  };
  if (auto res = upload_all(); !res) {
    std::cerr << "transaction " << id.str() << " left open, not committed\n";
    return res;
  }
  std::cout << "committed " << id.str() << " (" << files.size() << " files)\n";
  return {};
}

}  // namespace

const Command kSend{
    .name = "send",
    .synopsis = " --to USER[@SERVER] [--retention DAYS] [--comment TEXT] PATH...",
    .options = send_options,
    .run = run_send,
};

}  // namespace confide::cli
