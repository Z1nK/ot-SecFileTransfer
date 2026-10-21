#include "dispatch.hpp"

#include "commands/commands.hpp"
#include "password.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <iostream>
#include <string_view>

namespace confide::cli {

namespace {

constexpr std::string_view kCommands = R"(Commands:
  login                 ask for the password and save a token for later
                        commands (any command logs in first if needed)
  logout                end the session on the server and delete the token
  send --to USER[@SERVER] [--retention DAYS] [--comment TEXT] PATH...
                        create a transaction, upload the files and folders,
                        commit
  create --to USER[@SERVER] [--retention DAYS] [--comment TEXT]
                        create an open transaction and print its ID;
                        then add files and commit it in separate steps
  add ID PATH...        upload files and folders into an open transaction
                        (replaces a file with the same path)
  rm ID PATH...         remove files from an open transaction, by their
                        path in the transaction (see 'ftc files ID')
  commit ID             commit an open transaction; no changes after that
  inbox                 list transactions sent to you
  outbox                list transactions you sent
  show ID               show one transaction
  files ID              list the files of a transaction
  get ID [DIR] [--force]
                        download all files of a transaction into DIR
                        (default: current folder)

Run 'ftc COMMAND --help' for the options of a command.
)";

const std::array kAll{
    &kLogin, &kLogout, &kSend, &kCreate, &kAdd,   &kRm,
    &kCommit, &kInbox, &kOutbox, &kShow, &kFiles, &kGet,
};

const Command* find(std::string_view name) {
  const auto it = std::ranges::find(kAll, name, &Command::name);
  return it != kAll.end() ? *it : nullptr;
}

// Parses the arguments after the command name. Unknown options are errors
// here; the global options were taken out already.
po::variables_map parse_command(const std::vector<std::string>& args,
                                const po::options_description& desc,
                                const po::positional_options_description& pos) {
  po::variables_map vm;
  po::store(
      po::command_line_parser(args)
          .options(desc)
          .positional(pos)
          .style(po::command_line_style::default_style & ~po::command_line_style::allow_guessing)
          .run(),
      vm);
  po::notify(vm);
  return vm;
}

}  // namespace

void usage(const po::options_description& global) {
  std::cout << "usage: ftc [OPTIONS] COMMAND [ARGS]\n\n" << global << '\n' << kCommands;
}

int run_command(const std::string& command, const std::vector<std::string>& args,
                const Global& global) {
  const Command* cmd = find(command);
  if (cmd == nullptr) {
    std::cerr << "ftc: unknown command '" << command << "'\n\n" << kCommands;
    return kExitUsage;
  }

  po::options_description desc(std::format("usage: ftc {}{}\n\nOptions", command, cmd->synopsis));
  desc.add_options()("help,h", "show this help");
  po::options_description hidden;
  po::positional_options_description pos;
  if (cmd->options != nullptr) {
    cmd->options(desc, hidden, pos);
  }

  po::options_description all;
  all.add(desc).add(hidden);
  po::variables_map vm;
  try {
    // --help first, so it works without the required arguments.
    if (std::ranges::any_of(args, [](const auto& a) { return a == "-h" || a == "--help"; })) {
      std::cout << desc;
      return kExitOk;
    }
    vm = parse_command(args, all, pos);
  } catch (const po::error& e) {
    std::cerr << "ftc " << command << ": " << e.what() << "\n\n" << desc;
    return kExitUsage;
  }

  if (auto res = cmd->run(global, vm); !res) {
    std::cerr << "ftc " << command << ": " << describe(res.error()) << '\n';
    if (res.error().code == ErrCode::auth && cmd != &kLogin && !can_ask_password()) {
      std::cerr << "ftc: the session may have expired; run 'ftc login'\n";
    }
    return kExitFailed;
  }
  return kExitOk;
}

}  // namespace confide::cli
