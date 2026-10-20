// Server entry point (Architecture §4, `main`): loads the config, wires the
// modules together and runs the HTTP server until SIGINT or SIGTERM.
//
//   confide-server path/to/server.toml
//
// Only wiring lives here.

#include <api/auth_handlers.hpp>
#include <api/authenticator.hpp>
#include <api/handlers.hpp>
#include <api/router.hpp>
#include <api/server.hpp>
#include <auth/authenticator.hpp>
#include <auth/user_directory.hpp>
#include <common/error.hpp>
#include <common/time.hpp>
#include <config/config.hpp>
#include <forwarder/forwarder.hpp>
#include <logging/business_log.hpp>
#include <logging/sink.hpp>
#include <logging/tech_log.hpp>
#include <net/http_client.hpp>
#include <retention/scheduler.hpp>
#include <storage/storage.hpp>
#include <transaction/repository.hpp>
#include <transaction/service.hpp>
#include <version_constants.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

namespace asio = boost::asio;
namespace fs = std::filesystem;

using namespace confide;
using common::ErrCode;
using common::Error;
using common::Result;

constexpr std::string_view kTag = "main";

void usage(std::ostream& out, std::string_view prog) {
  out << "confide-server " << version::kProjectVersionString << "\n"
      << "usage: " << prog << " <config.toml>\n"
      << "       " << prog << " -h|--help\n"
      << "       " << prog << " -v|--version\n";
}

// "[code] detail", for stderr and the technical log.
std::string describe(const Error& err) {
  return std::format("[{}] {}", common::to_string(err.code), err.detail);
}

// FR-13: a local target must be a known user or group of the user
// directory, a remote one a configured peer. Whether the user exists on the
// peer is only known when forwarding.
transaction::DestinationCheck destination_check(const config::Config& cfg,
                                                const auth::IUserDirectory& directory) {
  return [&cfg, &directory](std::string_view user, std::string_view server) -> Result<void> {
    if (server.empty() || server == cfg.instance_name()) {
      CFD_TRY(is_user, directory.has_user(user));
      if (is_user) {
        return {};
      }
      CFD_TRY(is_group, directory.has_group(user));
      if (!is_group) {
        return std::unexpected(
            Error::make(ErrCode::unknown_destination,
                        std::format("no user or group '{}' on this server", user)));
      }
      return {};
    }
    if (cfg.find_peer(server) == nullptr) {
      return std::unexpected(
          Error::make(ErrCode::unknown_destination, std::format("unknown server '{}'", server)));
    }
    return {};
  };
}

// Peers from [[peer]], reached over https when [tls] is on (the same switch
// as our own API), verified with tls.ca.
forwarder::ForwarderOptions forwarder_options(const config::Config& cfg) {
  forwarder::ForwarderOptions options;
  for (const auto& p : cfg.peers()) {
    options.peers.push_back(forwarder::Peer{
        .name = p.name,
        .url = net::ServerUrl{.tls = cfg.tls().enabled, .host = p.host,
                              .port = std::to_string(p.port)},
        .token = p.token,
    });
  }
  if (!cfg.tls().ca_path.empty()) {
    options.ca_file = cfg.tls().ca_path;
  }
  return options;
}

// api::IAuthenticator on top of auth::Authenticator: hands it the
// Authorization header and turns the name it returns into a Caller.
class HeaderAuthenticator final : public api::IAuthenticator {
public:
  explicit HeaderAuthenticator(const auth::Authenticator& auth) : auth_(auth) {}

  Result<transaction::Caller> authenticate_user(const api::RequestHeader& req,
                                                std::string_view source) const override {
    CFD_TRY(name, auth_.authenticate_user(authorization(req), source));
    return transaction::Caller{.name = std::move(name), .is_peer = false};
  }

  Result<transaction::Caller> authenticate_password(const api::RequestHeader& req,
                                                    std::string_view source) const override {
    CFD_TRY(name, auth_.authenticate_password(authorization(req), source));
    return transaction::Caller{.name = std::move(name), .is_peer = false};
  }

  Result<transaction::Caller> authenticate_peer(const api::RequestHeader& req) const override {
    CFD_TRY(name, auth_.authenticate_peer(authorization(req)));
    return transaction::Caller{.name = std::move(name), .is_peer = true};
  }

  Result<api::IssuedToken> issue_token(std::string_view user) const override {
    CFD_TRY(issued, auth_.issue_token(user));
    return api::IssuedToken{.token = std::move(issued.token), .expires_at = issued.expires_at};
  }

  void revoke_token(const api::RequestHeader& req) const override {
    auth_.revoke_token(authorization(req));
  }

private:
  static std::string_view authorization(const api::RequestHeader& req) {
    const auto value = req[boost::beast::http::field::authorization];
    return {value.data(), value.size()};
  }

  const auth::Authenticator& auth_;
};

// Builds the services and serves until a signal arrives. The logs are
// created by the caller so that a failure here still reaches them.
Result<void> serve(const config::Config& cfg, const common::IClock& clock, logging::TechLog& tlog,
                   logging::BusinessLog& blog) {
  CFD_TRY(store, storage::FileStore::open(cfg.storage().root));

  // Local users or LDAP, per auth.provider. Owned here: the service and the
  // authenticator only hold references.
  CFD_TRY(directory, auth::make_directory(cfg, &tlog));

  transaction::TransactionRepository repo(store);
  CFD_TRYV(repo.load());

  // Set once the forwarder below exists, before the server takes requests,
  // so no commit can see it unset.
  forwarder::Forwarder* forward = nullptr;

  transaction::TransactionService service(
      repo, store, blog, clock,
      transaction::ServiceOptions{
          .local_server = cfg.instance_name(),
          .default_retention_days = static_cast<int>(cfg.retention().default_days),
          .max_retention_days = static_cast<int>(cfg.retention().max_days),
          .check_destination = destination_check(cfg, *directory),
          .is_recipient = [&dir = *directory](std::string_view user, std::string_view target) {
            return dir.is_member(user, target);
          },
          // Received transactions have no target_server, so only our own
          // remote ones are pushed.
          .on_committed =
              [&forward](const transaction::Transaction& tx) {
                if (forward != nullptr && !tx.target_server.empty()) {
                  forward->enqueue(tx.id);
                }
              },
      });

  forwarder::Forwarder forwarding(service, store, tlog, forwarder_options(cfg));
  forward = &forwarding;

  api::TransactionHandlers handlers(service, blog, tlog);
  api::Router router;
  CFD_TRYV(handlers.register_routes(router));

  CFD_TRY(users_and_peers, auth::Authenticator::create(cfg, *directory, clock));
  const HeaderAuthenticator authenticator(users_and_peers);
  api::AuthHandlers auth_handlers(authenticator, tlog);
  CFD_TRYV(auth_handlers.register_routes(router));

  CFD_TRY(options, api::ServerOptions::from_config(cfg));
  const bool tls = options.tls != nullptr;
  api::HttpServer server(std::move(options), router, authenticator, tlog);

  // Installed before start() so an early Ctrl+C is not lost.
  asio::io_context signal_io;
  asio::signal_set signals(signal_io, SIGINT, SIGTERM);
  signals.async_wait([&](const std::error_code& ec, int signo) {
    if (!ec) {
      tlog.info(kTag, "signal {}, shutting down", signo);
      server.stop();
    }
  });

  CFD_TRYV(server.start());

  // Sweeps once now (catches what expired while the server was down), then
  // every sweep_interval_s. Sweeps and requests share the per-transaction
  // locks in TransactionService, so they can run side by side.
  retention::RetentionScheduler retention(
      service, tlog, std::chrono::seconds{cfg.retention().sweep_interval_s});
  retention.start();
  // Picks up what was committed but not delivered before a restart.
  forwarding.start();
  if (!tls) {
    tlog.warn(kTag, "TLS is off ([tls] enabled = false): passwords travel unencrypted, "
                    "use for local testing only");
  }
  tlog.info(kTag, "{} listening on {}:{} ({})", cfg.instance_name(), cfg.rest().bind, server.port(),
            tls ? "https" : "http");

  signal_io.run();  // returns once the signal handler has run
  server.wait();
  // A push in progress is aborted and redone on the next start.
  forwarding.stop();
  retention.stop();
  tlog.info(kTag, "stopped");
  return {};
}

}  // namespace

int main(int argc, char** argv) {
  const std::span args(argv, static_cast<std::size_t>(argc));
  if (args.size() != 2) {
    usage(std::cerr, args[0]);
    return 2;
  }
  if (const std::string_view arg = args[1]; arg == "-h" || arg == "--help") {
    usage(std::cout, args[0]);
    return 0;
  } else if (arg == "-v" || arg == "--version") {
    std::cout << "confide-server " << version::kProjectVersionString << '\n';
    return 0;
  }

  auto cfg = config::ConfigLoader::from_file(args[1]);
  if (!cfg) {
    std::cerr << "config " << args[1] << ": " << describe(cfg.error()) << '\n';
    return 1;
  }

  // FileSink does not create its directory, and fails silently without it.
  const auto& log_cfg = cfg->log();
  std::error_code ec;
  fs::create_directories(log_cfg.dir, ec);
  if (ec) {
    std::cerr << "log dir " << log_cfg.dir.string() << ": " << ec.message() << '\n';
    return 1;
  }

  const common::SystemClock clock;
  logging::TechLog tlog(
      std::make_unique<logging::FileSink>(log_cfg.dir / "technical.log", log_cfg.rotate_bytes));
  tlog.set_level(log_cfg.level);
  logging::BusinessLog blog(
      std::make_unique<logging::FileSink>(log_cfg.dir / "business.log", log_cfg.rotate_bytes),
      clock);

  tlog.info(kTag, "confide-server {} starting, config {}", version::kProjectVersionString,
            args[1]);

  for (const auto& w : cfg->warnings()) {
    tlog.warn(kTag, "config: {}", w);
  }

  if (auto res = serve(*cfg, clock, tlog, blog); !res) {
    tlog.error(kTag, "startup failed: {}", describe(res.error()));
    std::cerr << "startup failed: " << describe(res.error()) << '\n';
    return 1;
  }
  return 0;
}
