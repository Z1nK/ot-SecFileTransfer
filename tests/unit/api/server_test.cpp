#include <api/auth_handlers.hpp>
#include <api/blocking.hpp>
#include <api/server.hpp>
#include <gtest/gtest.h>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <filesystem>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using confide::api::Access;
using confide::api::AuthHandlers;
using confide::api::HttpServer;
using confide::api::IAuthenticator;
using confide::api::IssuedToken;
using confide::api::RequestContext;
using confide::api::RequestHeader;
using confide::api::Response;
using confide::api::Route;
using confide::api::Router;
using confide::api::ServerOptions;
using confide::common::ErrCode;
using confide::common::Error;
using confide::common::RelativePath;
using confide::common::Result;
using confide::common::Sha256;
using confide::common::TransactionId;
using confide::storage::FileStore;
using confide::transaction::Caller;

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace fs = std::filesystem;
using tcp = asio::ip::tcp;

namespace {

class NullSink final : public confide::logging::ILogSink {
public:
  void write(confide::logging::LogLevel, std::string_view) override {}
};

// "Bearer user-token" -> alice, "Bearer peer-token" -> peer siteB.
class FakeAuth final : public IAuthenticator {
public:
  Result<Caller> authenticate_user(const RequestHeader& req,
                                   std::string_view source) const override {
    remember(source);
    if (req[http::field::authorization] == "Bearer user-token") {
      return Caller{.name = "alice"};
    }
    return std::unexpected(Error::make(ErrCode::auth, "bad credentials"));
  }

  Result<Caller> authenticate_password(const RequestHeader& req,
                                       std::string_view source) const override {
    remember(source);
    if (req[http::field::authorization] == "Basic good") {
      return Caller{.name = "alice"};
    }
    if (req[http::field::authorization] == "Basic slow") {
      return std::unexpected(Error::make(ErrCode::throttled, "retry in 30 s"));
    }
    return std::unexpected(Error::make(ErrCode::auth, "bad credentials"));
  }

  Result<Caller> authenticate_peer(const RequestHeader& req) const override {
    if (req[http::field::authorization] == "Bearer peer-token") {
      return Caller{.name = "siteB", .is_peer = true};
    }
    return std::unexpected(Error::make(ErrCode::auth, "bad peer token"));
  }

  // "Bearer admin-token" -> admin root; a user's token is forbidden.
  Result<Caller> authenticate_admin(const RequestHeader& req,
                                    std::string_view /*source*/) const override {
    if (req[http::field::authorization] == "Bearer admin-token") {
      return Caller{.name = "root", .is_admin = true};
    }
    if (req[http::field::authorization] == "Bearer user-token") {
      return std::unexpected(Error::make(ErrCode::forbidden, "admin rights required"));
    }
    return std::unexpected(Error::make(ErrCode::auth, "bad credentials"));
  }

  Result<IssuedToken> issue_token(std::string_view user) const override {
    return IssuedToken{.token = "ftu_for_" + std::string{user},
                       .expires_at = std::chrono::sys_days{std::chrono::year{2026} / 9 / 24}};
  }

  void revoke_token(const RequestHeader& req) const override {
    const std::scoped_lock lock(mutex_);
    revoked_ = std::string{req[http::field::authorization]};
  }

  std::string last_source() const {
    const std::scoped_lock lock(mutex_);
    return source_;
  }
  std::string revoked() const {
    const std::scoped_lock lock(mutex_);
    return revoked_;
  }

private:
  void remember(std::string_view source) const {
    const std::scoped_lock lock(mutex_);
    source_ = source;
  }

  mutable std::mutex mutex_;
  mutable std::string source_;
  mutable std::string revoked_;
};

std::string text(std::span<const std::byte> bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};  // NOLINT
}

std::string make_payload(std::size_t size) {
  std::string s(size, '\0');
  for (std::size_t i = 0; i < size; ++i) {
    s[i] = static_cast<char>('a' + (i % 26));
  }
  return s;
}

asio::awaitable<void> echo(RequestContext& ctx) {
  std::string body = "name=" + *ctx.param("name") + ";caller=" + ctx.caller.name +
                     ";peer=" + (ctx.caller.is_peer ? "1" : "0") +
                     ";box=" + std::string{ctx.query_value("box").value_or("-")};
  co_await ctx.exchange.send(Response{.body = std::move(body), .content_type = "text/plain"});
}

// Counts body bytes with a small buffer, so the body arrives in many reads.
asio::awaitable<void> count(RequestContext& ctx) {
  std::vector<std::byte> buf(4096);
  std::uint64_t total = 0;
  std::string checksum_input;
  while (true) {
    auto n = co_await ctx.exchange.read_body(buf);
    if (!n) {
      co_await ctx.exchange.send(Response::error(n.error()));
      co_return;
    }
    if (*n == 0) {
      break;
    }
    total += *n;
    checksum_input += text(std::span{buf}.first(*n));
  }
  co_await ctx.exchange.send(Response{
      .body = std::to_string(total) + ":" + Sha256::of(checksum_input).value().to_hex(),
      .content_type = "text/plain",
  });
}

asio::awaitable<void> json_echo(RequestContext& ctx) {
  auto body = co_await ctx.exchange.read_body_string(16);
  if (!body) {
    co_await ctx.exchange.send(Response::error(body.error()));
    co_return;
  }
  co_await ctx.exchange.send(Response::json(http::status::created, *body));
}

class ServerTest : public ::testing::Test {
protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::temp_directory_path() / (std::string{"confide_api_"} + info->name());
    fs::remove_all(dir_);
    store_.emplace(FileStore::open(dir_).value());
    ASSERT_TRUE(store_->create_transaction(id_));
    auto up = store_->begin_upload(id_, path_).value();
    ASSERT_TRUE(up.write(payload_));
    ASSERT_TRUE(up.commit());

    tlog_ = std::make_unique<confide::logging::TechLog>(std::make_unique<NullSink>());
    auth_handlers_ = std::make_unique<AuthHandlers>(auth_, *tlog_);
    ASSERT_TRUE(auth_handlers_->register_routes(router_));

    add(http::verb::get, "/echo/{name}", Access::user, echo);
    add(http::verb::post, "/peer/{name}", Access::peer, echo);
    add(http::verb::get, "/adm/{name}", Access::admin, echo);
    add(http::verb::post, "/adm-login/{name}", Access::admin_password, echo);
    add(http::verb::put, "/count", Access::user, count);
    add(http::verb::post, "/json", Access::user, json_echo);
    add(http::verb::get, "/file", Access::user, [this](RequestContext& ctx) -> asio::awaitable<void> {
      auto file = store_->open_read(id_, path_).value();
      const auto digest = Sha256::of(payload_).value();
      co_await ctx.exchange.send_file(file, digest);
    });
    add(http::verb::get, "/throw", Access::user, [](RequestContext&) -> asio::awaitable<void> {
      throw std::runtime_error("boom");
      co_return;
    });
    add(http::verb::get, "/silent", Access::user,
        [](RequestContext&) -> asio::awaitable<void> { co_return; });

    server_ = std::make_unique<HttpServer>(
        ServerOptions{.bind = "127.0.0.1", .port = 0, .threads = 2, .max_upload_bytes = 8 << 20},
        router_, auth_, *tlog_);
    ASSERT_TRUE(server_->start());
    ASSERT_NE(server_->port(), 0);
  }

  void TearDown() override {
    server_.reset();
    auth_handlers_.reset();
    tlog_.reset();
    store_.reset();
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  void add(http::verb method, std::string pattern, Access access, confide::api::Handler h) {
    ASSERT_TRUE(router_.add(Route{
        .method = method, .pattern = std::move(pattern), .access = access, .handler = std::move(h)}));
  }

  beast::tcp_stream connect() {
    asio::ip::tcp::endpoint ep{asio::ip::make_address("127.0.0.1"), server_->port()};
    beast::tcp_stream stream{ioc_};
    stream.connect(ep);
    stream.expires_after(std::chrono::seconds(10));
    return stream;
  }

  http::response<http::string_body> roundtrip(beast::tcp_stream& stream,
                                              http::request<http::string_body> req) {
    req.version(11);
    req.set(http::field::host, "localhost");
    req.prepare_payload();
    http::write(stream, req);
    http::response_parser<http::string_body> parser;
    parser.body_limit(64 << 20);
    http::read(stream, buffer_, parser);
    return parser.release();
  }

  http::response<http::string_body> call(http::verb method, std::string target,
                                         std::string body = {},
                                         std::string auth = "Bearer user-token") {
    auto stream = connect();
    http::request<http::string_body> req{method, target, 11};
    if (!auth.empty()) {
      req.set(http::field::authorization, auth);
    }
    req.body() = std::move(body);
    return roundtrip(stream, std::move(req));
  }

  fs::path dir_;
  std::optional<FileStore> store_;
  TransactionId id_ = TransactionId::generate();
  RelativePath path_ = RelativePath::parse("report/a.bin").value();
  std::string payload_ = make_payload((3 << 20) + 123);  // several storage chunks

  Router router_;
  FakeAuth auth_;
  std::unique_ptr<confide::logging::TechLog> tlog_;
  std::unique_ptr<AuthHandlers> auth_handlers_;
  std::unique_ptr<HttpServer> server_;

  asio::io_context ioc_;
  beast::flat_buffer buffer_;
};

}  // namespace

TEST_F(ServerTest, RoutesWithParamsAndQuery) {
  auto res = call(http::verb::get, "/echo/a%20b?box=out");
  EXPECT_EQ(res.result(), http::status::ok);
  EXPECT_EQ(res.body(), "name=a b;caller=alice;peer=0;box=out");
  EXPECT_EQ(res[http::field::server], "confide");
}

TEST_F(ServerTest, UnknownPathIs404) {
  auto res = call(http::verb::get, "/nope");
  EXPECT_EQ(res.result(), http::status::not_found);
  EXPECT_NE(res.body().find(R"("error":"notfound")"), std::string::npos);
}

TEST_F(ServerTest, WrongMethodIs405) {
  EXPECT_EQ(call(http::verb::delete_, "/echo/x").result(), http::status::method_not_allowed);
}

TEST_F(ServerTest, MissingAuthIs401WithChallenge) {
  auto res = call(http::verb::get, "/echo/x", {}, "");
  EXPECT_EQ(res.result(), http::status::unauthorized);
  EXPECT_EQ(res[http::field::www_authenticate], R"(Basic realm="confide")");
}

TEST_F(ServerTest, PeerRouteNeedsPeerToken) {
  EXPECT_EQ(call(http::verb::post, "/peer/x").result(), http::status::unauthorized);

  auto res = call(http::verb::post, "/peer/x", {}, "Bearer peer-token");
  EXPECT_EQ(res.result(), http::status::ok);
  EXPECT_EQ(res.body(), "name=x;caller=siteB;peer=1;box=-");
}

TEST_F(ServerTest, AdminRoutesNeedAnAdmin) {
  EXPECT_EQ(call(http::verb::get, "/adm/x", {}, "").result(), http::status::unauthorized);
  EXPECT_EQ(call(http::verb::get, "/adm/x").result(), http::status::forbidden);
  EXPECT_EQ(call(http::verb::get, "/adm/x", {}, "Bearer peer-token").result(),
            http::status::unauthorized);

  auto res = call(http::verb::get, "/adm/x", {}, "Bearer admin-token");
  EXPECT_EQ(res.result(), http::status::ok);
  EXPECT_EQ(res.body(), "name=x;caller=root;peer=0;box=-");
}

TEST_F(ServerTest, AdminPasswordRoutesHaveNoAdminsByDefault) {
  // FakeAuth keeps IAuthenticator's default: nobody is admin.
  EXPECT_EQ(call(http::verb::post, "/adm-login/x", {}, "Basic good").result(),
            http::status::forbidden);
}

TEST_F(ServerTest, LoginNeedsThePassword) {
  // A token can't buy a new token.
  auto res = call(http::verb::post, "/auth/token");
  EXPECT_EQ(res.result(), http::status::unauthorized);
  EXPECT_EQ(res[http::field::www_authenticate], R"(Basic realm="confide")");

  res = call(http::verb::post, "/auth/token", {}, "Basic good");
  EXPECT_EQ(res.result(), http::status::ok);
  EXPECT_EQ(res.body(),
            R"({"token":"ftu_for_alice","user":"alice","expires_at":"2026-09-24T00:00:00Z"})");
  EXPECT_EQ(res[http::field::cache_control], "no-store");
  EXPECT_EQ(auth_.last_source(), "127.0.0.1");
}

TEST_F(ServerTest, ThrottledLoginIs429) {
  auto res = call(http::verb::post, "/auth/token", {}, "Basic slow");
  EXPECT_EQ(res.result(), http::status::too_many_requests);
  EXPECT_NE(res.body().find(R"("error":"throttled")"), std::string::npos);
}

TEST_F(ServerTest, LogoutRevokesThePresentedToken) {
  auto res = call(http::verb::delete_, "/auth/token");
  EXPECT_EQ(res.result(), http::status::no_content);
  EXPECT_EQ(auth_.revoked(), "Bearer user-token");
}

TEST_F(ServerTest, StreamsUploadBody) {
  const auto body = make_payload((2 << 20) + 7);
  auto res = call(http::verb::put, "/count", body);
  EXPECT_EQ(res.result(), http::status::ok);
  EXPECT_EQ(res.body(), std::to_string(body.size()) + ":" + Sha256::of(body).value().to_hex());
}

TEST_F(ServerTest, StreamsChunkedUploadBody) {
  const auto body = make_payload(100'000);
  auto stream = connect();
  http::request<http::string_body> req{http::verb::put, "/count", 11};
  req.set(http::field::authorization, "Bearer user-token");
  req.set(http::field::host, "localhost");
  req.body() = body;
  req.chunked(true);
  http::write(stream, req);
  http::response<http::string_body> res;
  http::read(stream, buffer_, res);
  EXPECT_EQ(res.result(), http::status::ok);
  EXPECT_EQ(res.body(), std::to_string(body.size()) + ":" + Sha256::of(body).value().to_hex());
}

TEST_F(ServerTest, UploadOverLimitIs400) {
  // Rejected from the Content-Length alone, before any body byte is sent.
  auto stream = connect();
  http::request<http::empty_body> req{http::verb::put, "/count", 11};
  req.set(http::field::authorization, "Bearer user-token");
  req.set(http::field::host, "localhost");
  req.content_length((8 << 20) + 1);
  http::request_serializer<http::empty_body> sr{req};
  http::write_header(stream, sr);

  http::response<http::string_body> res;
  http::read(stream, buffer_, res);
  EXPECT_EQ(res.result(), http::status::bad_request);
  EXPECT_FALSE(res.keep_alive());
}

TEST_F(ServerTest, AnswersExpectContinue) {
  auto stream = connect();
  http::request<http::string_body> req{http::verb::put, "/count", 11};
  req.set(http::field::authorization, "Bearer user-token");
  req.set(http::field::host, "localhost");
  req.set(http::field::expect, "100-continue");
  req.body() = "hello";
  req.prepare_payload();

  http::request_serializer<http::string_body> sr{req};
  http::write_header(stream, sr);

  http::response<http::empty_body> interim;
  http::read(stream, buffer_, interim);
  EXPECT_EQ(interim.result(), http::status::continue_);

  http::write(stream, sr);
  http::response<http::string_body> res;
  http::read(stream, buffer_, res);
  EXPECT_EQ(res.result(), http::status::ok);
  EXPECT_EQ(res.body(), "5:" + Sha256::of(std::string_view{"hello"}).value().to_hex());
}

TEST_F(ServerTest, JsonBodyLimit) {
  auto ok = call(http::verb::post, "/json", R"({"a":1})");
  EXPECT_EQ(ok.result(), http::status::created);
  EXPECT_EQ(ok.body(), R"({"a":1})");
  EXPECT_EQ(ok[http::field::content_type], "application/json");

  EXPECT_EQ(call(http::verb::post, "/json", std::string(17, 'x')).result(),
            http::status::bad_request);
}

TEST_F(ServerTest, StreamsDownloadWithChecksum) {
  auto res = call(http::verb::get, "/file");
  EXPECT_EQ(res.result(), http::status::ok);
  EXPECT_EQ(res[http::field::content_type], "application/octet-stream");
  EXPECT_EQ(res["X-Checksum-SHA256"], Sha256::of(payload_).value().to_hex());
  EXPECT_EQ(res[http::field::content_length], std::to_string(payload_.size()));
  EXPECT_EQ(res.body(), payload_);
}

TEST_F(ServerTest, HandlerExceptionAndSilenceAre500) {
  EXPECT_EQ(call(http::verb::get, "/throw").result(), http::status::internal_server_error);
  EXPECT_EQ(call(http::verb::get, "/silent").result(), http::status::internal_server_error);
}

TEST_F(ServerTest, KeepAliveServesSeveralRequests) {
  auto stream = connect();
  for (int i = 0; i < 3; ++i) {
    http::request<http::string_body> req{http::verb::get, "/echo/k", 11};
    req.set(http::field::authorization, "Bearer user-token");
    auto res = roundtrip(stream, std::move(req));
    EXPECT_EQ(res.result(), http::status::ok);
    EXPECT_TRUE(res.keep_alive());
  }
  http::request<http::string_body> up{http::verb::put, "/count", 11};
  up.set(http::field::authorization, "Bearer user-token");
  up.body() = "abc";
  EXPECT_EQ(roundtrip(stream, std::move(up)).result(), http::status::ok);
}

TEST_F(ServerTest, UnreadBodyClosesConnection) {
  // 401 is sent before the body is read, so the connection can't be reused.
  auto res = call(http::verb::put, "/count", "abc", "");
  EXPECT_EQ(res.result(), http::status::unauthorized);
  EXPECT_FALSE(res.keep_alive());
}

TEST_F(ServerTest, MalformedRequestIs400) {
  auto stream = connect();
  const std::string garbage = "THIS IS NOT HTTP\r\n\r\n";
  asio::write(stream, asio::buffer(garbage));
  http::response<http::string_body> res;
  http::read(stream, buffer_, res);
  EXPECT_EQ(res.result(), http::status::bad_request);
}

TEST_F(ServerTest, StopClosesIdleConnections) {
  auto stream = connect();
  http::request<http::string_body> req{http::verb::get, "/echo/k", 11};
  req.set(http::field::authorization, "Bearer user-token");
  ASSERT_EQ(roundtrip(stream, std::move(req)).result(), http::status::ok);

  // The keep-alive connection is idle in read_header; stop() must not wait
  // for its 30 s timeout.
  const auto t0 = std::chrono::steady_clock::now();
  server_->stop();
  server_->wait();
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(5));

  http::response<http::string_body> res;
  beast::error_code ec;
  http::read(stream, buffer_, res, ec);
  EXPECT_TRUE(ec);  // closed by the server
}

TEST(ServerStart, UnresolvableBindHostFails) {
  Router router;
  FakeAuth auth;
  confide::logging::TechLog tlog{std::make_unique<NullSink>()};
  // .invalid never resolves (RFC 6761).
  HttpServer server{ServerOptions{.bind = "no-such-host.invalid", .port = 0}, router, auth, tlog};
  EXPECT_EQ(server.start().error().code, ErrCode::validation);
}

TEST(ServerStart, BindHostNameIsResolved) {
  Router router;
  FakeAuth auth;
  confide::logging::TechLog tlog{std::make_unique<NullSink>()};
  HttpServer server{ServerOptions{.bind = "localhost", .port = 0, .threads = 1}, router, auth,
                    tlog};
  ASSERT_TRUE(server.start());
  EXPECT_NE(server.port(), 0);
  server.stop();
  server.wait();
}

TEST(ServerStart, StartTwiceFails) {
  Router router;
  FakeAuth auth;
  confide::logging::TechLog tlog{std::make_unique<NullSink>()};
  HttpServer server{ServerOptions{.bind = "127.0.0.1", .port = 0, .threads = 1}, router, auth,
                    tlog};
  ASSERT_TRUE(server.start());
  EXPECT_EQ(server.start().error().code, ErrCode::internal);
  server.stop();
  server.wait();
}

// Two requests parked in offload() leave the only network thread free; the
// third blocking thread authenticates the echo request.
TEST(ServerBlocking, OffloadedCallsDoNotStallOtherRequests) {
  Router router;
  FakeAuth auth;
  confide::logging::TechLog tlog{std::make_unique<NullSink>()};
  std::latch started{2};
  std::latch release{1};
  ASSERT_TRUE(router.add(Route{
      .method = http::verb::get,
      .pattern = "/echo/{name}",
      .access = Access::user,
      .handler = echo,
  }));
  ASSERT_TRUE(router.add(Route{
      .method = http::verb::get,
      .pattern = "/block",
      .access = Access::user,
      .handler = [&](RequestContext& ctx) -> asio::awaitable<void> {
        co_await confide::api::offload(ctx.blocking, [&] {
          started.count_down();
          release.wait();
          return 0;
        });
        co_await ctx.exchange.send(Response{.body = "done", .content_type = "text/plain"});
      },
  }));
  HttpServer server{
      ServerOptions{.bind = "127.0.0.1", .port = 0, .threads = 1, .blocking_threads = 3}, router,
      auth, tlog};
  ASSERT_TRUE(server.start());

  const auto get = [port = server.port()](const std::string& target) {
    asio::io_context ioc;
    beast::tcp_stream stream{ioc};
    stream.connect(tcp::endpoint{asio::ip::make_address("127.0.0.1"), port});
    stream.expires_after(std::chrono::seconds(5));
    http::request<http::empty_body> req{http::verb::get, target, 11};
    req.set(http::field::host, "localhost");
    req.set(http::field::authorization, "Bearer user-token");
    http::write(stream, req);
    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);
    return res;
  };

  std::vector<std::string> blocked(2);
  std::vector<std::thread> clients;
  for (auto& body : blocked) {
    clients.emplace_back([&get, &body] { body = get("/block").body(); });
  }
  started.wait();

  EXPECT_EQ(get("/echo/x").result(), http::status::ok);

  release.count_down();
  for (auto& t : clients) {
    t.join();
  }
  EXPECT_EQ(blocked, (std::vector<std::string>{"done", "done"}));
  server.stop();
  server.wait();
}
