// ftc create, add, rm, commit: build a transaction step by step. `send` does
// all of it at once; these leave the transaction open between the steps.
#include "commands.hpp"
#include "local_files.hpp"
#include "outgoing.hpp"
#include "reply.hpp"
#include "session.hpp"

#include <common/path_sanitizer.hpp>

#include <boost/beast/http/verb.hpp>

#include <iostream>
#include <optional>
#include <utility>
#include <vector>

namespace confide::cli {

namespace {

using http_verb = boost::beast::http::verb;

// ID first, then one or more paths.
void id_and_paths(po::options_description& /*desc*/, po::options_description& hidden,
                  po::positional_options_description& pos) {
  hidden.add_options()("id", po::value<std::string>()->required())(
      "path", po::value<std::vector<std::string>>()->required());
  pos.add("id", 1).add("path", -1);
}

void create_options(po::options_description& desc, po::options_description& /*hidden*/,
                    po::positional_options_description& /*pos*/) {
  new_transaction_options(desc);
}

Result<void> run_create(const Global& global, const po::variables_map& vm) {
  const auto req = new_transaction_from(vm);
  CFD_TRY(client, connect(global));
  CFD_TRY(id, create_transaction(client, req));
  std::cout << "created " << id.str() << " for " << req.to << '\n';
  return {};
}

Result<void> run_add(const Global& global, const po::variables_map& vm) {
  CFD_TRY(id, common::TransactionId::parse(vm["id"].as<std::string>()));
  CFD_TRY(files, collect_files(vm["path"].as<std::vector<std::string>>()));
  CFD_TRY(client, connect(global));
  CFD_TRYV(upload_files(client, id, files));
  std::cout << "added " << files.size() << " files to " << id.str() << '\n';
  return {};
}

Result<void> run_rm(const Global& global, const po::variables_map& vm) {
  // Paths as the transaction has them (see `ftc files ID`), not local ones.
  CFD_TRY(id, common::TransactionId::parse(vm["id"].as<std::string>()));
  std::vector<common::RelativePath> paths;
  for (const auto& p : vm["path"].as<std::vector<std::string>>()) {
    CFD_TRY(rel, common::RelativePath::parse(p));
    paths.push_back(std::move(rel));
  }

  CFD_TRY(client, connect(global));
  for (const auto& rel : paths) {
    CFD_TRYV(client.call(http_verb::delete_,
                         tx_target(id) + "/files/" + net::url_encode_path(rel.str())));
    std::cout << "  " << rel.str() << '\n';
  }
  std::cout << "removed " << paths.size() << " files from " << id.str() << '\n';
  return {};
}

void id_only(po::options_description& /*desc*/, po::options_description& hidden,
             po::positional_options_description& pos) {
  hidden.add_options()("id", po::value<std::string>()->required());
  pos.add("id", 1);
}

Result<void> run_commit(const Global& global, const po::variables_map& vm) {
  CFD_TRY(id, common::TransactionId::parse(vm["id"].as<std::string>()));
  CFD_TRY(client, connect(global));
  CFD_TRY(tx, commit_transaction(client, id));
  std::cout << "committed " << id.str() << " (" << num(tx, "file_count") << " files, "
            << num(tx, "total_bytes") << " bytes)\n";
  return {};
}

}  // namespace

void new_transaction_options(po::options_description& desc) {
  desc.add_options()("to,t",
                     po::value<std::string>()->required()->value_name("USER|GROUP[@SERVER]"),
                     "receiver, a user or a group; without @SERVER one on the same server")(
      "retention,r", po::value<int>()->value_name("DAYS"),
      "days to keep the transaction (server default if not set)")(
      "comment,m", po::value<std::string>()->value_name("TEXT"),
      "short note shown in inbox/outbox (at most 256 characters)");
}

NewTransaction new_transaction_from(const po::variables_map& vm) {
  return {
      .to = vm["to"].as<std::string>(),
      .retention_days = vm.contains("retention") ? std::optional{vm["retention"].as<int>()}
                                                 : std::nullopt,
      .comment = vm.contains("comment") ? std::optional{vm["comment"].as<std::string>()}
                                        : std::nullopt,
  };
}

const Command kCreate{
    .name = "create",
    .synopsis = " --to USER[@SERVER] [--retention DAYS] [--comment TEXT]",
    .options = create_options,
    .run = run_create,
};
const Command kAdd{.name = "add", .synopsis = " ID PATH...", .options = id_and_paths, .run = run_add};
const Command kRm{.name = "rm", .synopsis = " ID PATH...", .options = id_and_paths, .run = run_rm};
const Command kCommit{.name = "commit", .synopsis = " ID", .options = id_only, .run = run_commit};

}  // namespace confide::cli
