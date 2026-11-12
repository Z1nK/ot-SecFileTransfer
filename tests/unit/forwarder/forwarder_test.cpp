#include <api/handlers.hpp>
#include <api/server.hpp>
#include <forwarder/forwarder.hpp>
#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using confide::api::HttpServer;
using confide::api::IAuthenticator;
using confide::api::IssuedToken;
using confide::api::RequestHeader;
using confide::api::Router;
using confide::api::ServerOptions;
using confide::api::TransactionHandlers;
using confide::common::ErrCode;
using confide::common::Error;
using confide::common::RelativePath;
using confide::common::Result;
using confide::common::TransactionId;
using confide::forwarder::Forwarder;
using confide::forwarder::ForwarderOptions;
using confide::forwarder::Peer;
using confide::net::ServerUrl;
using confide::storage::FileStore;
using confide::transaction::Caller;
using confide::transaction::CreateRequest;
using confide::transaction::ForwardStatus;
using confide::transaction::ReceiveRequest;
using confide::transaction::ServiceOptions;
using confide::transaction::State;
using confide::transaction::Transaction;
using confide::transaction::TransactionRepository;
using confide::transaction::TransactionService;

namespace asio = boost::asio;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

// Keeps the technical log, to see what the forwarder's worker did.
class LinesSink final : public confide::logging::ILogSink {
public:
  void write(confide::logging::LogLevel, std::string_view line) override {
    const std::scoped_lock lock(mu_);
    lines_.emplace_back(line);
  }

  bool contains(std::string_view text) const {
    const std::scoped_lock lock(mu_);
    return std::ranges::any_of(lines_, [&](const std::string& l) { return l.contains(text); });
  }

private:
  mutable std::mutex mu_;
  std::vector<std::string> lines_;
};

// Site B's view of callers: only peer siteA, with the token siteA sends.
class PeerOnlyAuth final : public IAuthenticator {
public:
  Result<Caller> authenticate_user(const RequestHeader&, std::string_view) const override {
    return std::unexpected(Error::make(ErrCode::auth, "users not needed here"));
  }
  Result<Caller> authenticate_password(const RequestHeader&, std::string_view) const override {
    return std::unexpected(Error::make(ErrCode::auth, "users not needed here"));
  }
  Result<Caller> authenticate_peer(const RequestHeader& req) const override {
    if (req[boost::beast::http::field::authorization] == "Bearer a-to-b") {
      return Caller{.name = "siteA", .is_peer = true};
    }
    return std::unexpected(Error::make(ErrCode::auth, "bad peer token"));
  }
  Result<IssuedToken> issue_token(std::string_view) const override {
    return std::unexpected(Error::make(ErrCode::internal, "not used"));
  }
  void revoke_token(const RequestHeader&) const override {}
};

// One server's storage, repository and service in its own temp folder.
struct Site {
  Site(const fs::path& dir, std::string name, confide::transaction::DestinationCheck check,
       confide::transaction::CommitHook on_committed)
      : store(FileStore::open(dir).value())
      , repo(store)
      , service(repo, store, blog, clock,
                ServiceOptions{
                    .local_server = std::move(name),
                    .check_destination = std::move(check),
                    .is_recipient = {},
                    .on_committed = on_committed,
                    .on_retry = on_committed,
                    .retry_min_interval = std::chrono::seconds{0},
                }) {}

  confide::common::SystemClock clock;
  LinesSink* events = new LinesSink;  // owned by blog
  confide::logging::BusinessLog blog{std::unique_ptr<LinesSink>(events), clock};
  FileStore store;
  TransactionRepository repo;
  TransactionService service;
};

// A local port nothing listens on.
std::string closed_port() {
  asio::io_context io;
  const asio::ip::tcp::acceptor acceptor{io, {asio::ip::make_address("127.0.0.1"), 0}};
  return std::to_string(acceptor.local_endpoint().port());
}

// Polls `done` for up to 10 s.
bool eventually(const std::function<bool()>& done) {
  for (int i = 0; i < 1000; ++i) {
    if (done()) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return done();
}

// alice@siteA sends to bob@siteB. Site B runs the real API on a free port;
// site A has a Forwarder pointing at it.
class ForwarderTest : public ::testing::Test {
protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::temp_directory_path() / (std::string{"confide_forwarder_"} + info->name());
    fs::remove_all(dir_);

    site_b_ = std::make_unique<Site>(
        dir_ / "b", "siteB",
        [](std::string_view user, std::string_view) -> Result<std::string> {
          if (user == "bob") {
            return std::string{user};
          }
          return std::unexpected(Error::make(ErrCode::unknown_destination, "no such user"));
        },
        nullptr);
    handlers_ = std::make_unique<TransactionHandlers>(site_b_->service, site_b_->blog, tlog_);
    ASSERT_TRUE(handlers_->register_routes(router_));
    server_ = std::make_unique<HttpServer>(
        ServerOptions{.bind = "127.0.0.1", .port = 0, .threads = 2}, router_, auth_, tlog_);
    ASSERT_TRUE(server_->start());

    site_a_ = std::make_unique<Site>(dir_ / "a", "siteA", nullptr, [this](const Transaction& tx) {
      if (forwarder_ && !tx.target_server.empty()) {
        forwarder_->enqueue(tx.id);
      }
    });
    make_forwarder(std::to_string(server_->port()));
  }

  void TearDown() override {
    forwarder_.reset();
    server_.reset();
    handlers_.reset();
    site_a_.reset();
    site_b_.reset();
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  void make_forwarder(const std::string& port, std::chrono::milliseconds io_timeout = 5s,
                      std::chrono::milliseconds first_retry = 50ms) {
    forwarder_.reset();
    forwarder_ = std::make_unique<Forwarder>(
        site_a_->service, site_a_->store, tlog_,
        ForwarderOptions{
            .peers = {Peer{.name = "siteB",
                           .url = ServerUrl{.tls = false, .host = "127.0.0.1", .port = port},
                           .token = "a-to-b"}},
            .io_timeout = io_timeout,
            .first_retry = first_retry,
            .max_retry = std::max<std::chrono::milliseconds>(first_retry, 200ms),
        });
  }

  // alice -> `to`@siteB with `files`, committed unless `commit` is false.
  Transaction send(const std::map<std::string, std::string>& files, bool commit = true,
                   std::string to = "bob") {
    auto& svc = site_a_->service;
    auto tx = svc.create(alice_, CreateRequest{.target_user = std::move(to),
                                               .target_server = "siteB",
                                               .comment = "quarterly"});
    EXPECT_TRUE(tx.has_value()) << tx.error().detail;
    for (const auto& [path, content] : files) {
      upload(svc, alice_, tx->id, path, content);
    }
    if (commit) {
      EXPECT_TRUE(svc.commit(alice_, tx->id).has_value());
    }
    return *tx;
  }

  static void upload(TransactionService& svc, const Caller& who, const TransactionId& id,
                     const std::string& path, std::string_view content) {
    auto up = svc.begin_upload(who, id, RelativePath::parse(path).value());
    ASSERT_TRUE(up.has_value()) << up.error().detail;
    ASSERT_TRUE(up->write(content).has_value());
    ASSERT_TRUE(up->finish().has_value());
  }

  // What site B received, as bob reads it.
  std::string read_on_b(const TransactionId& id, const std::string& path) const {
    auto file = site_b_->service.open_download(bob_, id, RelativePath::parse(path).value()).value();
    std::string out(static_cast<std::size_t>(file.size()), '\0');
    const auto n = file.read(std::as_writable_bytes(std::span{out})).value();
    out.resize(n);
    return out;
  }

  // Site B's copy as a fresh push from siteA would leave it, before commit.
  void receive_on_b(const Transaction& tx) const {
    ASSERT_TRUE(site_b_->service
                    .receive(peer_a_, ReceiveRequest{.id = tx.id,
                                                     .sender = tx.sender,
                                                     .target_user = tx.target_user,
                                                     .created_at = tx.created_at,
                                                     .expires_at = tx.expires_at})
                    .has_value());
  }

  State state_on_a(const TransactionId& id) const {
    return site_a_->service.get(alice_, id).value().state;
  }

  fs::path dir_;
  LinesSink* lines_ = new LinesSink;  // owned by tlog_
  confide::logging::TechLog tlog_{std::unique_ptr<LinesSink>(lines_)};
  PeerOnlyAuth auth_;
  Router router_;
  std::unique_ptr<Site> site_b_;
  std::unique_ptr<TransactionHandlers> handlers_;
  std::unique_ptr<HttpServer> server_;
  std::unique_ptr<Site> site_a_;
  std::unique_ptr<Forwarder> forwarder_;

  Caller alice_{.name = "alice"};
  Caller bob_{.name = "bob"};
  Caller peer_a_{.name = "siteA", .is_peer = true};
};

}  // namespace

TEST_F(ForwarderTest, PushDeliversFilesAndMarksDelivered) {
  const auto tx = send({{"a.txt", "hello"}, {"dir/b.bin", std::string(300'000, 'x')}});
  ASSERT_EQ(site_a_->service.list_outgoing(), std::vector{tx.id});

  const auto res = forwarder_->push(tx.id);
  ASSERT_TRUE(res.has_value()) << res.error().detail;

  EXPECT_EQ(state_on_a(tx.id), State::delivered);
  EXPECT_TRUE(site_a_->service.list_outgoing().empty());

  const auto there = site_b_->service.get(bob_, tx.id).value();
  EXPECT_EQ(there.state, State::committed);
  EXPECT_EQ(there.sender, "alice");
  EXPECT_EQ(there.origin_server, "siteA");
  EXPECT_EQ(there.target_server, "");
  EXPECT_EQ(there.comment, "quarterly");
  EXPECT_EQ(there.expires_at, tx.expires_at);
  EXPECT_EQ(there.files, site_a_->service.get(alice_, tx.id).value().files);
  EXPECT_EQ(read_on_b(tx.id, "a.txt"), "hello");
  EXPECT_EQ(read_on_b(tx.id, "dir/b.bin"), std::string(300'000, 'x'));
}

TEST_F(ForwarderTest, EmptyTransactionIsForwardedToo) {
  const auto tx = send({});
  ASSERT_TRUE(forwarder_->push(tx.id).has_value());
  EXPECT_EQ(state_on_a(tx.id), State::delivered);
  EXPECT_EQ(site_b_->service.get(bob_, tx.id).value().state, State::committed);
}

TEST_F(ForwarderTest, RetryCompletesAnInterruptedPush) {
  const auto tx = send({{"a.txt", "hello"}, {"b.txt", "world"}});
  // An earlier attempt created it on B and got half a file across.
  receive_on_b(tx);
  upload(site_b_->service, peer_a_, tx.id, "a.txt", "hel");

  const auto res = forwarder_->push(tx.id);
  ASSERT_TRUE(res.has_value()) << res.error().detail;
  EXPECT_EQ(state_on_a(tx.id), State::delivered);
  EXPECT_EQ(read_on_b(tx.id, "a.txt"), "hello");
  EXPECT_EQ(read_on_b(tx.id, "b.txt"), "world");
}

TEST_F(ForwarderTest, RetryAfterALostConfirmationOnlyMarksDelivered) {
  const auto tx = send({{"a.txt", "hello"}});
  // An earlier attempt went through, but its answer never arrived.
  receive_on_b(tx);
  upload(site_b_->service, peer_a_, tx.id, "a.txt", "hello");
  ASSERT_TRUE(site_b_->service.commit(peer_a_, tx.id).has_value());

  const auto res = forwarder_->push(tx.id);
  ASSERT_TRUE(res.has_value()) << res.error().detail;
  EXPECT_EQ(state_on_a(tx.id), State::delivered);
}

TEST_F(ForwarderTest, OnlyCommittedOutgoingTransactionsArePushed) {
  const auto open = send({{"a.txt", "x"}}, false);
  EXPECT_EQ(forwarder_->push(open.id).error().code, ErrCode::conflict);

  const auto local =
      site_a_->service.create(alice_, CreateRequest{.target_user = "bob", .target_server = "siteA"})
          .value();
  ASSERT_TRUE(site_a_->service.commit(alice_, local.id).has_value());
  EXPECT_EQ(forwarder_->push(local.id).error().code, ErrCode::conflict);

  EXPECT_EQ(forwarder_->push(TransactionId::generate()).error().code, ErrCode::notfound);
}

TEST_F(ForwarderTest, UnknownUserOnThePeerIsNotRetried) {
  const auto tx = send({{"a.txt", "hello"}}, true, "carol");
  const auto res = forwarder_->push(tx.id);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code, ErrCode::unknown_destination);
  EXPECT_FALSE(Forwarder::is_transient(res.error().code));
  EXPECT_EQ(state_on_a(tx.id), State::committed);  // The peer logs the push it refused.
  // TechLog writes on its own thread: wait for the line.
  EXPECT_TRUE(eventually([&] {
    return lines_->contains("POST /internal/transactions from peer siteA: unknown_destination");
  }));
}

TEST_F(ForwarderTest, WrongPeerTokenIsNotRetried) {
  forwarder_ = std::make_unique<Forwarder>(
      site_a_->service, site_a_->store, tlog_,
      ForwarderOptions{.peers = {Peer{.name = "siteB",
                                      .url = ServerUrl{.tls = false,
                                                       .host = "127.0.0.1",
                                                       .port = std::to_string(server_->port())},
                                      .token = "wrong"}}});
  const auto tx = send({{"a.txt", "hello"}});
  const auto res = forwarder_->push(tx.id);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code, ErrCode::auth);
  EXPECT_FALSE(Forwarder::is_transient(res.error().code));
  EXPECT_TRUE(eventually([&] {
    return lines_->contains("POST /internal/transactions from 127.0.0.1: bad peer token");
  }));
}

TEST_F(ForwarderTest, PeerDownIsTransient) {
  make_forwarder(closed_port());
  const auto tx = send({{"a.txt", "hello"}});
  const auto res = forwarder_->push(tx.id);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code, ErrCode::unavailable);
  EXPECT_TRUE(Forwarder::is_transient(res.error().code));
  EXPECT_EQ(state_on_a(tx.id), State::committed);
}

TEST_F(ForwarderTest, WorkerPushesOnCommit) {
  forwarder_->start();
  const auto tx = send({{"a.txt", "hello"}});
  EXPECT_TRUE(eventually([&] { return state_on_a(tx.id) == State::delivered; }));
  EXPECT_EQ(read_on_b(tx.id, "a.txt"), "hello");
}

TEST_F(ForwarderTest, StartPicksUpWhatWasLeftBefore) {
  // Committed while no forwarder ran, e.g. before a restart.
  auto saved = std::move(forwarder_);
  const auto first = send({{"a.txt", "1"}});
  const auto second = send({{"a.txt", "2"}});
  forwarder_ = std::move(saved);

  forwarder_->start();
  EXPECT_TRUE(eventually([&] {
    return state_on_a(first.id) == State::delivered && state_on_a(second.id) == State::delivered;
  }));
}

TEST_F(ForwarderTest, WorkerKeepsRetryingWhileThePeerIsDown) {
  make_forwarder(closed_port());
  forwarder_->start();
  const auto tx = send({{"a.txt", "hello"}});
  // Tried again after each of the 50-200 ms backoffs; the log has the first
  // failure and every 10th, not each one.
  EXPECT_TRUE(eventually([&] { return lines_->contains("attempt 10 failed"); }));
  EXPECT_TRUE(lines_->contains("attempt 1 failed"));
  EXPECT_FALSE(lines_->contains("attempt 2 failed"));
  EXPECT_FALSE(lines_->contains("attempt 9 failed"));
  EXPECT_EQ(state_on_a(tx.id), State::committed);
  EXPECT_FALSE(site_a_->events->contains("forward_failed"));
  // The sender sees why it has not arrived yet. The attempt is recorded
  // just after its log line, so wait for it.
  EXPECT_TRUE(eventually([&] {
    const auto seen = site_a_->service.get(alice_, tx.id).value().forwarding;
    return seen && seen->attempts >= 10;
  }));
  const auto fwd = site_a_->service.get(alice_, tx.id).value().forwarding;
  ASSERT_TRUE(fwd);
  EXPECT_EQ(fwd->status, ForwardStatus::retrying);
  EXPECT_GE(fwd->attempts, 10U);
  EXPECT_EQ(fwd->error, "unavailable");
  EXPECT_TRUE(fwd->next_attempt_at);
}

TEST_F(ForwarderTest, DeliveryAfterFailuresSaysSo) {
  // Site B is down at first, then comes up on the port the forwarder tries.
  const auto port = closed_port();
  make_forwarder(port);
  forwarder_->start();
  const auto tx = send({{"a.txt", "hello"}});
  EXPECT_TRUE(eventually([&] { return lines_->contains("attempt 1 failed"); }));

  HttpServer late(ServerOptions{.bind = "127.0.0.1",
                                .port = static_cast<std::uint16_t>(std::stoi(port)),
                                .threads = 1},
                  router_, auth_, tlog_);
  ASSERT_TRUE(late.start());
  EXPECT_TRUE(eventually([&] { return state_on_a(tx.id) == State::delivered; }));
  EXPECT_TRUE(eventually([&] { return lines_->contains(tx.id.str() + " delivered after "); }));
  forwarder_.reset();
}

TEST_F(ForwarderTest, PermanentFailureLeavesTheQueue) {
  forwarder_->start();
  const auto tx = send({{"a.txt", "hello"}}, true, "carol");
  EXPECT_TRUE(eventually([&] { return forwarder_->queued() == 0; }));
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(forwarder_->queued(), 0U);
  EXPECT_EQ(state_on_a(tx.id), State::committed);
  EXPECT_TRUE(eventually(
      [&] { return lines_->contains("not forwarded, waits for a retry or the next start"); }));
  // Recorded just after that log line.
  EXPECT_TRUE(eventually([&] {
    const auto seen = site_a_->service.get(alice_, tx.id).value().forwarding;
    return seen && seen->status == ForwardStatus::failed;
  }));
  const auto fwd = site_a_->service.get(alice_, tx.id).value().forwarding;
  ASSERT_TRUE(fwd);
  EXPECT_EQ(fwd->status, ForwardStatus::failed);
  EXPECT_EQ(fwd->attempts, 1U);
  EXPECT_EQ(fwd->error, "unknown_destination");
  EXPECT_TRUE(eventually([&] {
    return site_a_->events->contains(
        R"("event":"forward_failed","tx":")" + tx.id.str()
        + R"(","actor":"system","from":"alice","to":"carol","server":"siteB",)"
          R"("error":"unknown_destination","detail":")");
  }));
}

// A retry asked for while the forwarder waits out a long backoff goes now.
TEST_F(ForwarderTest, RetryCutsTheBackoffShort) {
  const auto port = closed_port();
  make_forwarder(port, 5s, 1h);
  forwarder_->start();
  const auto tx = send({{"a.txt", "hello"}});
  EXPECT_TRUE(eventually([&] { return lines_->contains("attempt 1 failed, retry in 3600s"); }));
  // Back in the queue a moment after that log line, once the attempt is recorded.
  EXPECT_TRUE(eventually([&] { return forwarder_->queued() == 1; }));

  HttpServer late(ServerOptions{.bind = "127.0.0.1",
                                .port = static_cast<std::uint16_t>(std::stoi(port)),
                                .threads = 1},
                  router_, auth_, tlog_);
  ASSERT_TRUE(late.start());
  ASSERT_TRUE(site_a_->service.retry_forward(alice_, tx.id).has_value());  // -> enqueue
  EXPECT_LE(forwarder_->queued(), 1U);  // moved up (or taken already), not added twice
  EXPECT_TRUE(eventually([&] { return state_on_a(tx.id) == State::delivered; }));
  EXPECT_FALSE(site_a_->service.get(alice_, tx.id).value().forwarding);
  forwarder_.reset();
}

// A permanent failure leaves the queue; a retry once it is fixed brings it back.
TEST_F(ForwarderTest, RetryAfterAFailureIsPushedAgain) {
  forwarder_->start();
  const auto tx = send({{"a.txt", "hello"}}, true, "carol");
  EXPECT_TRUE(eventually([&] {
    const auto fwd = site_a_->service.get(alice_, tx.id).value().forwarding;
    return fwd && fwd->status == ForwardStatus::failed;
  }));
  ASSERT_TRUE(site_a_->service.retry_forward(alice_, tx.id).has_value());
  // carol still does not exist on B: it fails again, one attempt more.
  EXPECT_TRUE(eventually([&] {
    const auto fwd = site_a_->service.get(alice_, tx.id).value().forwarding;
    return fwd && fwd->status == ForwardStatus::failed && fwd->attempts == 2;
  }));
}

// A retry that comes in while the push it wants repeated is still the active
// one is not lost when that push then fails for good: it is queued once more,
// after first_retry.
TEST_F(ForwarderTest, RetryDuringAPushIsNotLost) {
  // A peer that takes the request and refuses it only when told to.
  asio::io_context io;
  asio::ip::tcp::acceptor acceptor{io, {asio::ip::make_address("127.0.0.1"), 0}};
  std::promise<void> answer;
  std::thread peer([&] {
    boost::system::error_code ec;
    auto socket = acceptor.accept(ec);
    std::array<char, 4096> request{};
    socket.read_some(asio::buffer(request), ec);
    answer.get_future().wait();
    const std::string_view body = R"({"error":"unknown_destination","detail":"no such user"})";
    const auto reply = std::format("HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\n"
                                   "Content-Length: {}\r\nConnection: close\r\n\r\n{}",
                                   body.size(), body);
    asio::write(socket, asio::buffer(reply), ec);
    socket.shutdown(asio::ip::tcp::socket::shutdown_send, ec);
    while (socket.read_some(asio::buffer(request), ec) > 0) {
    }
  });
  make_forwarder(std::to_string(acceptor.local_endpoint().port()), 5s, 1h);
  forwarder_->start();
  const auto tx = send({{"a.txt", "hello"}}, true, "carol");
  EXPECT_TRUE(eventually([&] { return forwarder_->queued() == 0; }));  // push in progress

  forwarder_->enqueue(tx.id);
  EXPECT_EQ(forwarder_->queued(), 0U);  // not twice while it is active
  answer.set_value();
  EXPECT_TRUE(eventually([&] {
    const auto fwd = site_a_->service.get(alice_, tx.id).value().forwarding;
    return fwd && fwd->status == ForwardStatus::failed;
  }));
  // Failed for good, yet back in the queue: the retry was asked for.
  EXPECT_TRUE(eventually([&] { return forwarder_->queued() == 1; }));
  forwarder_.reset();
  peer.join();
}

TEST_F(ForwarderTest, EnqueueDoesNotDuplicateTheTransactionBeingPushed) {
  // Accepts connections but never answers: the push stays in progress.
  asio::io_context io;
  const asio::ip::tcp::acceptor silent{io, {asio::ip::make_address("127.0.0.1"), 0}};
  make_forwarder(std::to_string(silent.local_endpoint().port()), 1h);
  forwarder_->start();
  const auto tx = send({{"a.txt", "hello"}});
  EXPECT_TRUE(eventually([&] { return forwarder_->queued() == 0; }));  // push in progress

  forwarder_->enqueue(tx.id);
  EXPECT_EQ(forwarder_->queued(), 0U);
  forwarder_->stop();
  // Cancelled by stop(), which is not a failure to show the sender.
  EXPECT_FALSE(site_a_->service.get(alice_, tx.id).value().forwarding);
}

TEST_F(ForwarderTest, StopAbortsAStalledPush) {
  // Accepts connections but never answers; no I/O timeout to save us.
  asio::io_context io;
  const asio::ip::tcp::acceptor silent{io, {asio::ip::make_address("127.0.0.1"), 0}};
  make_forwarder(std::to_string(silent.local_endpoint().port()), 1h);
  forwarder_->start();
  const auto tx = send({{"a.txt", "hello"}});
  EXPECT_TRUE(eventually([&] { return forwarder_->queued() == 0; }));  // push in progress

  const auto start = std::chrono::steady_clock::now();
  forwarder_->stop();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
  EXPECT_EQ(state_on_a(tx.id), State::committed);
}
