// ftc send: create a transaction, upload, commit, all in one go.
// ftc recent: the recipients `send` was last used with.
#include "commands.hpp"
#include <client/local_files.hpp>
#include <client/outgoing.hpp>
#include <client/recent_recipients.hpp>
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

Result<RecentRecipients> recent_for(const Global& global) {
  if (global.server.empty()) {
    return fail(ErrCode::validation, "no server: use --server or FTC_SERVER");
  }
  CFD_TRY(url, net::ServerUrl::parse(global.server));
  return RecentRecipients::for_server(url, global.user);
}

// After a send that worked; a failure here is only worth a warning.
void remember(const Global& global, const std::string& to) {
  auto recent = recent_for(global);
  auto res = recent ? recent->push(to) : Result<void>{std::unexpected(recent.error())};
  if (!res) {
    std::cerr << "ftc: warning: recipient not added to the recent list ("
              << describe(res.error()) << ")\n";
  }
}

Result<void> run_send(const Global& global, const po::variables_map& vm) {
  const auto req = new_transaction_from(vm);
  CFD_TRY(files, collect_files(vm["path"].as<std::vector<std::string>>()));
  CFD_TRY(client, connect(global));

  CFD_TRY(id, create_transaction(client, req));
  std::cout << "created " << id.str() << " for " << req.to << '\n';

  // The transaction stays open on failure; say which one it is.
  auto upload_all = [&]() -> Result<void> {
    CFD_TRYV(upload_files(client, id, files, print_uploaded));
    CFD_TRYV(commit_transaction(client, id));
    return {};
  };
  if (auto res = upload_all(); !res) {
    std::cerr << "transaction " << id.str() << " left open, not committed\n";
    return res;
  }
  std::cout << "committed " << id.str() << " (" << files.size() << " files)\n";
  remember(global, req.to);
  return {};
}

// One recipient per line, most recent first, for scripts and file manager
// menus. Nothing is printed if there are none yet.
Result<void> run_recent(const Global& global, const po::variables_map& /*vm*/) {
  CFD_TRY(recent, recent_for(global));
  CFD_TRY(list, recent.load());
  for (const auto& r : list) {
    std::cout << r << '\n';
  }
  return {};
}

}  // namespace

const Command kSend{
    .name = "send",
    .synopsis = " --to USER[@SERVER] [--retention DAYS] [--comment TEXT] PATH...",
    .options = send_options,
    .run = run_send,
};

const Command kRecent{.name = "recent", .synopsis = "", .run = run_recent};

}  // namespace confide::cli
