#pragma once
// Picking a command by name, parsing its arguments and running it.
#include "cli.hpp"

#include <boost/program_options/options_description.hpp>

#include <string>
#include <vector>

namespace confide::cli {

// Prints the top-level usage: `global` options and the command list.
void usage(const boost::program_options::options_description& global);

// Runs `command` with `args` (everything after the command name); returns
// the exit code.
int run_command(const std::string& command, const std::vector<std::string>& args,
                const Global& global);

}  // namespace confide::cli
