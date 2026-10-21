#pragma once
// The ftc commands. Each one declares its own options and runs against the
// server; dispatch.cpp picks one by name, parses its arguments and runs it.
#include "cli.hpp"
#include "outgoing.hpp"

#include <boost/program_options.hpp>

#include <string>
#include <string_view>

namespace confide::cli {

namespace po = boost::program_options;

struct Command {
  std::string_view name;
  std::string_view synopsis;  // after "ftc NAME" in the usage line

  // Adds the command's options to `desc` (shown in --help) and its
  // positional arguments to `hidden` and `pos`. Null if there are none.
  void (*options)(po::options_description& desc, po::options_description& hidden,
                  po::positional_options_description& pos) = nullptr;

  Result<void> (*run)(const Global& global, const po::variables_map& vm) = nullptr;
};

// commands/auth.cpp
extern const Command kLogin;
extern const Command kLogout;

// commands/send.cpp
extern const Command kSend;

// commands/compose.cpp
extern const Command kCreate;
extern const Command kAdd;
extern const Command kRm;
extern const Command kCommit;

// --to, --retention, --comment of `send` and `create`.
void new_transaction_options(po::options_description& desc);
NewTransaction new_transaction_from(const po::variables_map& vm);

// commands/query.cpp
extern const Command kInbox;
extern const Command kOutbox;
extern const Command kShow;
extern const Command kFiles;

// commands/get.cpp
extern const Command kGet;

}  // namespace confide::cli
