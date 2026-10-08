// `ftc`: command-line client for the REST API (Architecture §2, §4 `client`).
//
//   ftc -s http://siteA:8080 -u alice send --to bob@siteB ~/data/report
//   ftc -s http://siteB:8080 -u bob inbox
//   ftc -s http://siteB:8080 -u bob get <id> ~/downloads
//
// The password comes from FTC_PASSWORD, or is asked for on the terminal
// (first line of stdin if stdin is not a terminal), so it never appears in
// `ps` or the shell history.

#include "http_client.hpp"

#include <common/error.hpp>
#include <common/id.hpp>
#include <common/path_sanitizer.hpp>
#include <common/sha256.hpp>
#include <termios.h>
#include <unistd.h>

#include <boost/beast/http/verb.hpp>
#include <boost/json.hpp>
#include <boost/program_options.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace json = boost::json;
namespace po = boost::program_options;
using http_verb = boost::beast::http::verb;

using namespace confide;
using cli::HttpClient;
using common::ErrCode;
using common::Error;
using common::Result;

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;

constexpr std::size_t kHashChunkBytes = std::size_t{1} << 20;

constexpr std::string_view kCommands = R"(Commands:
  send --to USER[@SERVER] [--retention DAYS] PATH...
                        create a transaction, upload the files and folders,
                        commit
  inbox                 list transactions sent to you
  outbox                list transactions you sent
  show ID               show one transaction
  files ID              list the files of a transaction
  get ID [DIR] [--force]
                        download all files of a transaction into DIR
                        (default: current folder)

Run 'ftc COMMAND --help' for the options of a command.
)";

std::unexpected<Error> fail(ErrCode code, std::string detail) {
  return std::unexpected(Error::make(code, std::move(detail)));
}

std::string describe(const Error& err) {
  return std::format("[{}] {}", common::to_string(err.code), err.detail);
}

// --- Options ----------------------------------------------------------------

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

// Options shared by every command, from the command line or the environment.
struct Global {
  std::string server;
  std::string user;
  std::optional<fs::path> ca_file;
};

Result<std::string> read_password(std::string_view user) {
  if (const char* env = std::getenv("FTC_PASSWORD");
      env != nullptr) {  // NOLINT(concurrency-mt-unsafe)
    return std::string{env};
  }

  std::string password;
  if (isatty(STDIN_FILENO) == 0) {
    std::getline(std::cin, password);
  } else {
    std::cerr << "password for " << user << ": " << std::flush;
    termios old{};
    tcgetattr(STDIN_FILENO, &old);
    termios quiet = old;
    quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
    std::getline(std::cin, password);
    tcsetattr(STDIN_FILENO, TCSANOW, &old);
    std::cerr << '\n';
  }
  if (password.ends_with('\r')) {
    password.pop_back();
  }
  if (password.empty()) {
    return fail(ErrCode::validation, "no password (set FTC_PASSWORD or type it)");
  }
  return password;
}

Result<HttpClient> connect(const Global& global) {
  if (global.server.empty()) {
    return fail(ErrCode::validation, "no server: use --server or FTC_SERVER");
  }
  if (global.user.empty()) {
    return fail(ErrCode::validation, "no user: use --user or FTC_USER");
  }
  CFD_TRY(url, cli::ServerUrl::parse(global.server));
  CFD_TRY(password, read_password(global.user));
  return HttpClient::create(std::move(url), {.user = global.user, .password = std::move(password)},
                            global.ca_file);
}

// --- JSON replies -----------------------------------------------------------

Result<json::object> parse_object(std::string_view text) {
  boost::system::error_code ec;
  json::value v = json::parse(text, ec);
  if (ec || !v.is_object()) {
    return fail(ErrCode::internal, "server sent malformed JSON");
  }
  return std::move(v.get_object());
}

// "" if absent or not a string.
std::string_view str(const json::object& obj, std::string_view key) {
  const auto* v = obj.if_contains(key);
  return v != nullptr && v->is_string() ? std::string_view{v->get_string()} : std::string_view{};
}

std::uint64_t num(const json::object& obj, std::string_view key) {
  const auto* v = obj.if_contains(key);
  if (v == nullptr) {
    return 0;
  }
  if (v->is_uint64()) {
    return v->get_uint64();
  }
  return v->is_int64() && v->get_int64() > 0 ? static_cast<std::uint64_t>(v->get_int64()) : 0;
}

// The array under `key` in a list reply, e.g. {"files":[...]}.
Result<json::array> parse_list(std::string_view text, std::string_view key) {
  CFD_TRY(obj, parse_object(text));
  auto* arr = obj.if_contains(key);
  if (arr == nullptr || !arr->is_array()) {
    return fail(ErrCode::internal, std::format("server reply has no '{}' list", key));
  }
  return std::move(arr->get_array());
}

std::string address(const json::object& tx, std::string_view user_key,
                    std::string_view server_key) {
  std::string out{str(tx, user_key)};
  if (const auto server = str(tx, server_key); !server.empty()) {
    out += '@';
    out += server;
  }
  return out;
}

// --- Helpers ----------------------------------------------------------------

Result<common::Sha256Digest> hash_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return fail(ErrCode::io, "cannot open " + path.string());
  }
  CFD_TRY(sha, common::Sha256::create());
  std::vector<char> buf(kHashChunkBytes);
  while (in) {
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    CFD_TRYV(sha.update(std::string_view{buf.data(), static_cast<std::size_t>(in.gcount())}));
  }
  if (in.bad()) {
    return fail(ErrCode::io, "read error in " + path.string());
  }
  return sha.finish();
}

std::string tx_target(const common::TransactionId& id) {
  return "/transactions/" + id.str();
}

struct LocalFile {
  fs::path source;
  common::RelativePath path;
};

// Files to send. A folder keeps its own name as the first path component
// ("report/a.csv"), a single file is sent under its file name.
Result<std::vector<LocalFile>> collect_files(const std::vector<std::string>& args) {
  std::vector<LocalFile> out;
  std::set<std::string> seen;
  auto add = [&](const fs::path& base, const fs::path& file) -> Result<void> {
    CFD_TRY(rel, common::RelativePath::from_local(base, file));
    if (!seen.insert(rel.str()).second) {
      return fail(ErrCode::validation, "two files would be sent as " + rel.str());
    }
    out.push_back({.source = file, .path = std::move(rel)});
    return {};
  };

  for (const auto& arg : args) {
    std::error_code ec;
    auto path = fs::absolute(arg, ec).lexically_normal();
    if (!path.has_filename()) {  // "dir/"
      path = path.parent_path();
    }
    const auto base = path.parent_path();
    if (fs::is_regular_file(path, ec)) {
      CFD_TRYV(add(base, path));
    } else if (fs::is_directory(path, ec)) {
      for (fs::recursive_directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) {
          CFD_TRYV(add(base, it->path()));
        }
      }
      if (ec) {
        return fail(ErrCode::io, std::format("{}: {}", path.string(), ec.message()));
      }
    } else {
      return fail(ErrCode::validation, arg + ": no such file or folder");
    }
  }
  if (out.empty()) {
    return fail(ErrCode::validation, "nothing to send");
  }
  std::ranges::sort(out, {}, [](const LocalFile& f) { return f.path.str(); });
  return out;
}

// --- Commands ---------------------------------------------------------------

Result<void> cmd_send(const Global& global, const po::variables_map& vm) {
  const auto to = vm["to"].as<std::string>();
  const auto at = to.rfind('@');
  json::object body{{"target_user", to.substr(0, at)}};
  if (at != std::string::npos) {
    body["target_server"] = to.substr(at + 1);
  }
  if (vm.contains("retention")) {
    body["retention_days"] = vm["retention"].as<int>();
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
      CFD_TRYV(client.upload(tx_target(id) + "/files/" + cli::url_encode_path(file.path.str()),
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

Result<void> cmd_list(const Global& global, std::string_view box) {
  CFD_TRY(client, connect(global));
  CFD_TRY(reply, client.call(http_verb::get, std::format("/transactions?box={}", box)));
  CFD_TRY(list, parse_list(reply, "transactions"));

  const bool in = box == "in";
  std::cout << std::format("{:<36}  {:<9}  {:<24}  {:>5}  {:>12}  {}\n", "ID", "STATE",
                           in ? "FROM" : "TO", "FILES", "BYTES", "CREATED");
  for (const auto& v : list) {
    if (!v.is_object()) {
      continue;
    }
    const auto& tx = v.get_object();
    const auto peer =
        in ? std::string{str(tx, "sender")} : address(tx, "target_user", "target_server");
    std::cout << std::format("{:<36}  {:<9}  {:<24}  {:>5}  {:>12}  {}\n", str(tx, "id"),
                             str(tx, "state"), peer, num(tx, "file_count"), num(tx, "total_bytes"),
                             str(tx, "created_at"));
  }
  return {};
}

Result<void> cmd_show(const Global& global, const po::variables_map& vm) {
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

Result<void> cmd_files(const Global& global, const po::variables_map& vm) {
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

Result<void> cmd_get(const Global& global, const po::variables_map& vm) {
  CFD_TRY(id, common::TransactionId::parse(vm["id"].as<std::string>()));
  const fs::path dir = vm["dir"].as<std::string>();
  const bool force = vm.contains("force");

  CFD_TRY(client, connect(global));
  CFD_TRY(reply, client.call(http_verb::get, tx_target(id) + "/files"));
  CFD_TRY(list, parse_list(reply, "files"));

  for (const auto& v : list) {
    if (!v.is_object()) {
      return fail(ErrCode::internal, "server sent a malformed file list");
    }
    const auto& f = v.get_object();
    // Paths from the server are untrusted: no "../" may escape `dir`.
    CFD_TRY(rel, common::RelativePath::parse(str(f, "path")));
    CFD_TRY(expected, common::Sha256Digest::from_hex(str(f, "sha256")));

    const auto out = rel.under(dir);
    std::error_code ec;
    if (!force && fs::exists(out, ec)) {
      return fail(ErrCode::conflict, out.string() + " exists (use --force to overwrite)");
    }
    fs::create_directories(out.parent_path(), ec);
    if (ec) {
      return fail(ErrCode::io, std::format("{}: {}", out.parent_path().string(), ec.message()));
    }

    // Downloaded next to the target and renamed once the checksum matches.
    auto part = out;
    part += ".part";
    auto checked = [&]() -> Result<void> {
      CFD_TRYV(client.download(tx_target(id) + "/files/" + cli::url_encode_path(rel.str()), part));
      CFD_TRY(actual, hash_file(part));
      if (actual != expected) {
        return fail(ErrCode::integrity, rel.str() + ": SHA-256 does not match the server's");
      }
      fs::rename(part, out, ec);
      if (ec) {
        return fail(ErrCode::io, std::format("{}: {}", out.string(), ec.message()));
      }
      return {};
    };
    if (auto res = checked(); !res) {
      fs::remove(part, ec);
      return res;
    }
    std::cout << "  " << out.string() << '\n';
  }
  std::cout << "downloaded " << list.size() << " files into " << dir.string() << '\n';
  return {};
}

// --- Dispatch ---------------------------------------------------------------

void usage(const po::options_description& global) {
  std::cout << "usage: ftc [OPTIONS] COMMAND [ARGS]\n\n" << global << '\n' << kCommands;
}

// Runs `command` with `args`; returns the exit code.
int run_command(const std::string& command, const std::vector<std::string>& args,
                const Global& global) {
  const auto synopsis = [&]() -> std::string_view {
    if (command == "send") {
      return " --to USER[@SERVER] [--retention DAYS] PATH...";
    }
    if (command == "show" || command == "files") {
      return " ID";
    }
    if (command == "get") {
      return " ID [DIR] [--force]";
    }
    return "";
  };
  po::options_description desc(std::format("usage: ftc {}{}\n\nOptions", command, synopsis()));
  desc.add_options()("help,h", "show this help");
  po::options_description hidden;
  po::positional_options_description pos;
  Result<void> (*run)(const Global&, const po::variables_map&) = nullptr;

  if (command == "send") {
    desc.add_options()("to,t", po::value<std::string>()->required()->value_name("USER[@SERVER]"),
                       "receiver; without @SERVER a user on the same server")(
        "retention,r", po::value<int>()->value_name("DAYS"),
        "days to keep the transaction (server default if not set)");
    hidden.add_options()("path", po::value<std::vector<std::string>>()->required());
    pos.add("path", -1);
    run = cmd_send;
  } else if (command == "inbox") {
    run = [](const Global& g, const po::variables_map&) { return cmd_list(g, "in"); };
  } else if (command == "outbox") {
    run = [](const Global& g, const po::variables_map&) { return cmd_list(g, "out"); };
  } else if (command == "show" || command == "files") {
    hidden.add_options()("id", po::value<std::string>()->required());
    pos.add("id", 1);
    run = command == "show" ? cmd_show : cmd_files;
  } else if (command == "get") {
    desc.add_options()("force,f", "overwrite existing files");
    hidden.add_options()("id", po::value<std::string>()->required())(
        "dir", po::value<std::string>()->default_value("."));
    pos.add("id", 1).add("dir", 1);
    run = cmd_get;
  } else {
    std::cerr << "ftc: unknown command '" << command << "'\n\n" << kCommands;
    return kExitUsage;
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

  if (auto res = run(global, vm); !res) {
    std::cerr << "ftc " << command << ": " << describe(res.error()) << '\n';
    return kExitFailed;
  }
  return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
  po::options_description global("Options");
  global.add_options()("help,h", "show this help")(
      "server,s", po::value<std::string>()->value_name("URL"),
      "server, e.g. https://siteA:8080 (env FTC_SERVER)")(
      "user,u", po::value<std::string>()->value_name("NAME"), "user name (env FTC_USER)")(
      "ca-file", po::value<std::string>()->value_name("PATH"),
      "PEM CA bundle for https (env FTC_CA_FILE; default: system CAs)");
  po::options_description hidden;
  hidden.add_options()("command", po::value<std::string>())("args",
                                                            po::value<std::vector<std::string>>());
  po::options_description all;
  all.add(global).add(hidden);
  po::positional_options_description pos;
  pos.add("command", 1).add("args", -1);

  po::variables_map vm;
  std::vector<std::string> args;
  try {
    // Everything from the command name on belongs to the command.
    const auto parsed =
        po::command_line_parser(argc, argv)
            .options(all)
            .positional(pos)
            .style(po::command_line_style::default_style & ~po::command_line_style::allow_guessing)
            .allow_unregistered()
            .run();
    po::store(parsed, vm);
    po::store(po::parse_environment(global,
                                    [](const std::string& env) -> std::string {
                                      if (env == "FTC_SERVER") {
                                        return "server";
                                      }
                                      if (env == "FTC_USER") {
                                        return "user";
                                      }
                                      if (env == "FTC_CA_FILE") {
                                        return "ca-file";
                                      }
                                      return {};
                                    }),
              vm);
    po::notify(vm);

    args = po::collect_unrecognized(parsed.options, po::include_positional);
    if (!args.empty()) {
      args.erase(args.begin());  // the command name itself
    }
  } catch (const po::error& e) {
    std::cerr << "ftc: " << e.what() << "\n\n";
    usage(global);
    return kExitUsage;
  }

  if (!vm.contains("command")) {
    usage(global);
    return vm.contains("help") ? kExitOk : kExitUsage;
  }
  if (vm.contains("help")) {
    args.emplace_back("--help");
  }

  Global g{
      .server = vm.contains("server") ? vm["server"].as<std::string>() : std::string{},
      .user = vm.contains("user") ? vm["user"].as<std::string>() : std::string{},
      .ca_file = vm.contains("ca-file") && !vm["ca-file"].as<std::string>().empty()
                     ? std::optional<fs::path>{vm["ca-file"].as<std::string>()}
                     : std::nullopt,
  };
  return run_command(vm["command"].as<std::string>(), args, g);
}
