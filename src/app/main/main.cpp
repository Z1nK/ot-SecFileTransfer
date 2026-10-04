// Server entry point (Architecture §4, `main`): loads the config, wires the
// modules together and runs the HTTP server until SIGINT or SIGTERM.
//
//   confide-server path/to/server.toml
//
// Only wiring lives here. forwarder and retention are added once they exist;
// until then committed remote transactions stay `committed` and nothing
// expires.

#include <api/handlers.hpp>
#include <api/router.hpp>
#include <api/server.hpp>
#include <auth/authenticator.hpp>
#include <common/error.hpp>
#include <common/time.hpp>
#include <config/config.hpp>
#include <logging/business_log.hpp>
#include <logging/sink.hpp>
#include <logging/tech_log.hpp>
#include <storage/storage.hpp>
#include <transaction/repository.hpp>
#include <transaction/service.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <csignal>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>

namespace {

namespace asio = boost::asio;
namespace fs = std::filesystem;

using namespace confide;
using common::ErrCode;
using common::Error;
using common::Result;

constexpr std::string_view kTag = "main";

void usage(std::string_view prog) {
  std::cerr << "usage: " << prog << " <config.toml>\n";
}

// "[code] detail", for stderr and the technical log.
std::string describe(const Error& err) {
  return std::format("[{}] {}", common::to_string(err.code), err.detail);
}

// FR-13: a local target must be a configured user, a remote one a configured
// peer. Whether the user exists on the peer is only known when forwarding.
transaction::DestinationCheck destination_check(const config::Config& cfg) {
  return [&cfg](std::string_view user, std::string_view server) -> Result<void> {
    if (server.empty() || server == cfg.instance_name()) {
      if (cfg.find_user(user) == nullptr) {
        return std::unexpected(Error::make(ErrCode::unknown_destination,
                                           std::format("no user '{}' on this server", user)));
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

// Builds the services and serves until a signal arrives. The logs are
// created by the caller so that a failure here still reaches them.
Result<void> serve(const config::Config& cfg, const common::IClock& clock, logging::TechLog& tlog,
                   logging::BusinessLog& blog) {
  CFD_TRY(store, storage::FileStore::open(cfg.storage().root));

  transaction::TransactionRepository repo(store);
  CFD_TRYV(repo.load());

  transaction::TransactionService service(
      repo, store, blog, clock,
      transaction::ServiceOptions{
          .local_server = cfg.instance_name(),
          .default_retention_days = static_cast<int>(cfg.retention().default_days),
          .check_destination = destination_check(cfg),
          .on_committed = {},  // ForwardQueue::enqueue once forwarder exists
      });

  api::TransactionHandlers handlers(service, blog, tlog);
  api::Router router;
  CFD_TRYV(handlers.register_routes(router));

  const auth::Authenticator authenticator(cfg);

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
  tlog.info(kTag, "{} listening on {}:{} ({})", cfg.instance_name(), cfg.rest().bind, server.port(),
            tls ? "https" : "http");

  signal_io.run();  // returns once the signal handler has run
  server.wait();
  tlog.info(kTag, "stopped");
  return {};
}

}  // namespace

int main(int argc, char** argv) {
  const std::span args(argv, static_cast<std::size_t>(argc));
  if (args.size() != 2 || std::string_view(args[1]) == "-h"
      || std::string_view(args[1]) == "--help") {
    usage(args[0]);
    return 2;
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
