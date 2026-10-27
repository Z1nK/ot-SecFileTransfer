// ftc admin ...: the /admin/* API, for members of the server's admin group.
// Uses its own admin token (POST /admin/token), kept apart from the user
// token, so `ftc login` alone never grants admin rights.
#include "commands.hpp"
#include "reply.hpp"
#include "session.hpp"

#include <common/time.hpp>
#include <unistd.h>

#include <boost/beast/http/verb.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <iostream>
#include <string>

namespace confide::cli {

namespace {

using http_verb = boost::beast::http::verb;

constexpr std::string_view kAdminCommands =
    R"(Admin commands (for members of the server's admin group):
  login                 ask for the password and save an admin token
  logout                end the admin session and delete the token
  list [--who NAME] [--state STATE] [--from TIME] [--to TIME]
                        list all transactions (metadata only); --who
                        matches sender or target, --from/--to the creation
                        time
  show ID               show one transaction with its file list
  delete ID [--yes]     delete a transaction and its files
  retention ID DAYS     set the retention to DAYS from creation
  status                server status: version, peers, counts, sessions
  sessions              list the live sessions
  kick USER             end every session of USER
  log --from TIME [--to TIME] [--limit N]
                        business log lines in the time range

TIME is YYYY-MM-DD, YYYY-MM-DDTHH:MM:SSZ (UTC), now, or an age such as
30m, 2h, 7d, 1w. Run 'ftc admin COMMAND --help' for the options of a command.
)";

// "?a=1&b=2" from the non-empty pairs, URL-encoded; "" if none.
std::string query(std::initializer_list<std::pair<std::string_view, std::string>> pairs) {
  std::string out;
  for (const auto& [key, value] : pairs) {
    if (value.empty()) {
      continue;
    }
    out += out.empty() ? '?' : '&';
    out += key;
    out += '=';
    out += net::url_encode_path(value);
  }
  return out;
}

// A --from/--to value as the server expects it; "" if the option is absent.
Result<std::string> time_option(const po::variables_map& vm, const char* key) {
  if (!vm.contains(key)) {
    return std::string{};
  }
  CFD_TRY(ts, common::parse_time_arg(vm[key].as<std::string>(), common::SystemClock{}.now()));
  return common::format_iso8601(ts);
}

std::string admin_target(const common::TransactionId& id) {
  return "/admin/transactions/" + id.str();
}

void print_object(const json::object& obj) {
  for (const auto& [key, value] : obj) {
    if (key == "files") {
      continue;
    }
    std::cout << std::format(
        "{:<14} {}\n", std::string_view{key},
        value.is_string() ? std::string{value.get_string()} : json::serialize(value));
  }
}

// --- login / logout ----------------------------------------------------------

Result<void> run_login(const Global& global, const po::variables_map& /*vm*/) {
  CFD_TRY(session, open_session(global, Scope::admin));
  CFD_TRY(reply, login(session.client, global.user, session.cache, Scope::admin));
  std::cout << "admin session for " << reply.user << " until " << reply.expires_at << '\n';
  return {};
}

Result<void> run_logout(const Global& global, const po::variables_map& /*vm*/) {
  CFD_TRY(session, open_session(global, Scope::admin));
  CFD_TRY(saved, session.cache.load());
  if (!saved) {
    std::cout << "no admin session\n";
    return {};
  }
  session.client.set_token(*saved);
  auto revoked = session.client.logout(token_target(Scope::admin));
  CFD_TRYV(session.cache.remove());
  if (!revoked && revoked.error().code != ErrCode::auth) {
    std::cerr << "ftc: warning: server not told (" << describe(revoked.error())
              << "); the token stays valid there until it expires\n";
  }
  std::cout << "admin session ended\n";
  return {};
}

// --- transactions --------------------------------------------------------------

void list_options(po::options_description& desc, po::options_description& /*hidden*/,
                  po::positional_options_description& /*pos*/) {
  // Not --user: that is the global option naming who runs ftc.
  desc.add_options()("who", po::value<std::string>()->value_name("NAME"),
                     "only transactions sent by or to NAME")(
      "state", po::value<std::string>()->value_name("STATE"),
      "open, committed, delivered or expired")("from", po::value<std::string>()->value_name("TIME"),
                                               "created at or after TIME")(
      "to", po::value<std::string>()->value_name("TIME"), "created before TIME");
}

Result<void> run_list(const Global& global, const po::variables_map& vm) {
  CFD_TRY(from, time_option(vm, "from"));
  CFD_TRY(to, time_option(vm, "to"));
  const auto opt = [&](const char* key) {
    return vm.contains(key) ? vm[key].as<std::string>() : std::string{};
  };
  CFD_TRY(client, connect(global, Scope::admin));
  CFD_TRY(reply, client.call(http_verb::get, "/admin/transactions"
                                                 + query({{"user", opt("who")},
                                                          {"state", opt("state")},
                                                          {"from", from},
                                                          {"to", to}})));
  CFD_TRY(list, parse_list(reply, "transactions"));

  constexpr std::string_view kRow = "{:<36}  {:<9}  {:<20}  {:<24}  {:>5}  {:>12}  {:<20}  {}\n";
  std::cout << std::format(kRow, "ID", "STATE", "FROM", "TO", "FILES", "BYTES", "CREATED",
                           "EXPIRES");
  for (const auto& v : list) {
    if (!v.is_object()) {
      continue;
    }
    const auto& tx = v.get_object();
    std::cout << std::format(kRow, str(tx, "id"), str(tx, "state"),
                             address(tx, "sender", "origin_server"),
                             address(tx, "target_user", "target_server"), num(tx, "file_count"),
                             num(tx, "total_bytes"), str(tx, "created_at"), str(tx, "expires_at"));
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
  CFD_TRY(client, connect(global, Scope::admin));
  CFD_TRY(reply, client.call(http_verb::get, admin_target(id)));
  CFD_TRY(tx, parse_object(reply));
  print_object(tx);
  if (const auto* files = tx.if_contains("files"); files != nullptr && files->is_array()) {
    std::cout << "files:\n";
    for (const auto& v : files->get_array()) {
      if (v.is_object()) {
        const auto& f = v.get_object();
        std::cout << std::format("  {:>12}  {}  {}\n", num(f, "size"), str(f, "sha256"),
                                 str(f, "path"));
      }
    }
  }
  return {};
}

void delete_options(po::options_description& desc, po::options_description& hidden,
                    po::positional_options_description& pos) {
  desc.add_options()("yes,y", "don't ask for confirmation");
  id_option(desc, hidden, pos);
}

// Asks on the terminal; refuses without one, so a script must say --yes.
Result<void> confirm(const std::string& question) {
  if (isatty(STDIN_FILENO) == 0) {
    return fail(ErrCode::validation, "not a terminal: add --yes to confirm");
  }
  std::cerr << question << " [y/N] ";
  std::string answer;
  std::getline(std::cin, answer);
  if (answer != "y" && answer != "Y" && answer != "yes") {
    return fail(ErrCode::validation, "cancelled");
  }
  return {};
}

Result<void> run_delete(const Global& global, const po::variables_map& vm) {
  CFD_TRY(id, common::TransactionId::parse(vm["id"].as<std::string>()));
  if (!vm.contains("yes")) {
    CFD_TRYV(confirm(std::format("delete transaction {} and all its files?", id.str())));
  }
  CFD_TRY(client, connect(global, Scope::admin));
  CFD_TRYV(client.call(http_verb::delete_, admin_target(id)));
  std::cout << "deleted " << id.str() << '\n';
  return {};
}

void retention_options(po::options_description& /*desc*/, po::options_description& hidden,
                       po::positional_options_description& pos) {
  hidden.add_options()("id", po::value<std::string>()->required())("days",
                                                                   po::value<int>()->required());
  pos.add("id", 1).add("days", 1);
}

Result<void> run_retention(const Global& global, const po::variables_map& vm) {
  CFD_TRY(id, common::TransactionId::parse(vm["id"].as<std::string>()));
  const int days = vm["days"].as<int>();
  CFD_TRY(client, connect(global, Scope::admin));
  CFD_TRY(reply, client.call(http_verb::put, admin_target(id) + "/retention",
                             std::format(R"({{"days":{}}})", days)));
  CFD_TRY(tx, parse_object(reply));
  std::cout << id.str() << " now expires at " << str(tx, "expires_at") << '\n';
  return {};
}

// --- server ------------------------------------------------------------------

Result<void> run_status(const Global& global, const po::variables_map& /*vm*/) {
  CFD_TRY(client, connect(global, Scope::admin));
  CFD_TRY(reply, client.call(http_verb::get, "/admin/status"));
  CFD_TRY(st, parse_object(reply));
  std::cout << std::format("{:<14} {}\n", "instance", str(st, "instance"))
            << std::format("{:<14} {}\n", "version", str(st, "version"))
            << std::format("{:<14} {}\n", "auth", str(st, "auth_provider"))
            << std::format("{:<14} {} (up {} s)\n", "started", str(st, "started_at"),
                           num(st, "uptime_s"))
            << std::format("{:<14} {}\n", "sessions", num(st, "sessions"))
            << std::format("{:<14} {}\n", "to forward", num(st, "outgoing"));
  if (const auto* counts = st.if_contains("transactions");
      counts != nullptr && counts->is_object()) {
    std::cout << "transactions:\n";
    for (const auto& [state, n] : counts->get_object()) {
      std::cout << std::format("  {:<12} {}\n", std::string_view{state}, json::serialize(n));
    }
  }
  if (const auto* peers = st.if_contains("peers"); peers != nullptr && peers->is_array()) {
    std::cout << "peers:\n";
    for (const auto& v : peers->get_array()) {
      if (v.is_object()) {
        const auto& p = v.get_object();
        std::cout << std::format("  {:<12} {}:{}\n", str(p, "name"), str(p, "host"),
                                 num(p, "port"));
      }
    }
  }
  return {};
}

Result<void> run_sessions(const Global& global, const po::variables_map& /*vm*/) {
  CFD_TRY(client, connect(global, Scope::admin));
  CFD_TRY(reply, client.call(http_verb::get, "/admin/sessions"));
  CFD_TRY(list, parse_list(reply, "sessions"));
  constexpr std::string_view kRow = "{:<24}  {:<5}  {:<20}  {:<20}  {}\n";
  std::cout << std::format(kRow, "USER", "SCOPE", "LOGIN", "LAST USED", "EXPIRES");
  for (const auto& v : list) {
    if (v.is_object()) {
      const auto& s = v.get_object();
      std::cout << std::format(kRow, str(s, "user"), str(s, "scope"), str(s, "created"),
                               str(s, "last_used"), str(s, "expires_at"));
    }
  }
  return {};
}

void user_option(po::options_description& /*desc*/, po::options_description& hidden,
                 po::positional_options_description& pos) {
  hidden.add_options()("name", po::value<std::string>()->required());
  pos.add("name", 1);
}

Result<void> run_kick(const Global& global, const po::variables_map& vm) {
  const auto& user = vm["name"].as<std::string>();
  CFD_TRY(client, connect(global, Scope::admin));
  CFD_TRY(reply, client.call(http_verb::delete_, "/admin/sessions/" + net::url_encode_path(user)));
  CFD_TRY(res, parse_object(reply));
  std::cout << std::format("ended {} session(s) of {}\n", num(res, "revoked"), user);
  return {};
}

void log_options(po::options_description& desc, po::options_description& /*hidden*/,
                 po::positional_options_description& /*pos*/) {
  desc.add_options()("from", po::value<std::string>()->value_name("TIME")->required(),
                     "first time to include")("to", po::value<std::string>()->value_name("TIME"),
                                              "end, not included (default: now)")(
      "limit", po::value<unsigned>()->value_name("N"), "at most N lines (server default 1000)");
}

Result<void> run_log(const Global& global, const po::variables_map& vm) {
  CFD_TRY(from, time_option(vm, "from"));
  CFD_TRY(to, time_option(vm, "to"));
  const std::string limit = vm.contains("limit") ? std::to_string(vm["limit"].as<unsigned>()) : "";
  CFD_TRY(client, connect(global, Scope::admin));
  CFD_TRY(reply, client.call(http_verb::get,
                             "/admin/log" + query({{"from", from}, {"to", to}, {"limit", limit}})));
  std::cout << reply;
  return {};
}

const Command kAdminLogin{.name = "login", .synopsis = "", .run = run_login};
const Command kAdminLogout{.name = "logout", .synopsis = "", .run = run_logout};
const Command kAdminList{
    .name = "list", .synopsis = " [OPTIONS]", .options = list_options, .run = run_list};
const Command kAdminShow{.name = "show", .synopsis = " ID", .options = id_option, .run = run_show};
const Command kAdminDelete{
    .name = "delete", .synopsis = " ID [--yes]", .options = delete_options, .run = run_delete};
const Command kAdminRetention{.name = "retention",
                              .synopsis = " ID DAYS",
                              .options = retention_options,
                              .run = run_retention};
const Command kAdminStatus{.name = "status", .synopsis = "", .run = run_status};
const Command kAdminSessions{.name = "sessions", .synopsis = "", .run = run_sessions};
const Command kAdminKick{
    .name = "kick", .synopsis = " USER", .options = user_option, .run = run_kick};
const Command kAdminLog{
    .name = "log", .synopsis = " --from TIME [OPTIONS]", .options = log_options, .run = run_log};

const std::array kAdminAll{
    &kAdminLogin,     &kAdminLogout, &kAdminList,     &kAdminShow, &kAdminDelete,
    &kAdminRetention, &kAdminStatus, &kAdminSessions, &kAdminKick, &kAdminLog,
};

}  // namespace

int run_admin(const std::vector<std::string>& args, const Global& global) {
  if (args.empty() || args.front() == "-h" || args.front() == "--help") {
    std::cout << "usage: ftc [OPTIONS] admin COMMAND [ARGS]\n\n" << kAdminCommands;
    return args.empty() ? kExitUsage : kExitOk;
  }
  const auto& name = args.front();
  const auto it = std::ranges::find(kAdminAll, std::string_view{name}, &Command::name);
  if (it == kAdminAll.end()) {
    std::cerr << "ftc admin: unknown command '" << name << "'\n\n" << kAdminCommands;
    return kExitUsage;
  }
  return run_parsed(**it, "ftc admin " + name, "ftc admin login",
                    {std::next(args.begin()), args.end()}, global);
}

}  // namespace confide::cli
