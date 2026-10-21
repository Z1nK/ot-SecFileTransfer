// ftc send: create a transaction, upload, commit, all in one go.
#include "commands.hpp"
#include "local_files.hpp"
#include "outgoing.hpp"
#include "session.hpp"

#include <iostream>
#include <vector>

namespace confide::cli {

namespace {

void send_options(po::options_description& desc, po::options_description& hidden,
                  po::positional_options_description& pos) {
  new_transaction_options(desc);
  hidden.add_options()("path", po::value<std::vector<std::string>>()->required());
  pos.add("path", -1);
}

Result<void> run_send(const Global& global, const po::variables_map& vm) {
  const auto req = new_transaction_from(vm);
  CFD_TRY(files, collect_files(vm["path"].as<std::vector<std::string>>()));
  CFD_TRY(client, connect(global));

  CFD_TRY(id, create_transaction(client, req));
  std::cout << "created " << id.str() << " for " << req.to << '\n';

  // The transaction stays open on failure; say which one it is.
  auto upload_all = [&]() -> Result<void> {
    CFD_TRYV(upload_files(client, id, files));
    CFD_TRYV(commit_transaction(client, id));
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
