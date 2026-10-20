#include <gtest/gtest.h>
#include <net/http_client.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http/verb.hpp>

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <thread>

using confide::common::ErrCode;
using confide::common::Error;
using confide::common::Result;
using confide::common::Sha256Digest;
using confide::net::BodySource;
using confide::net::HttpClient;
using confide::net::ServerUrl;
using confide::net::url_encode_path;

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using http_verb = boost::beast::http::verb;
using namespace std::chrono_literals;

namespace {

// Listens but never accepts: the kernel completes the TCP handshake, takes
// what fits in its buffers, and nothing ever answers.
class SilentServer {
public:
  std::string port() const { return std::to_string(acceptor_.local_endpoint().port()); }

  ServerUrl url() const { return ServerUrl{.tls = false, .host = "127.0.0.1", .port = port()}; }

private:
  asio::io_context io_;
  tcp::acceptor acceptor_{io_, tcp::endpoint{asio::ip::make_address("127.0.0.1"), 0}};
};

}  // namespace

TEST(ServerUrl, Parses) {
  const auto url = ServerUrl::parse("https://siteA.example.org:9443/").value();
  EXPECT_TRUE(url.tls);
  EXPECT_EQ(url.host, "siteA.example.org");
  EXPECT_EQ(url.port, "9443");

  const auto plain = ServerUrl::parse("http://localhost").value();
  EXPECT_FALSE(plain.tls);
  EXPECT_EQ(plain.port, "8080");
}

TEST(ServerUrl, RejectsBadInput) {
  for (const auto* text : {"ftp://host", "http://", "http://host/path", "http://host:0",
                           "http://host:65536", "http://host:12ab"}) {
    const auto url = ServerUrl::parse(text);
    ASSERT_FALSE(url.has_value()) << text;
    EXPECT_EQ(url.error().code, ErrCode::validation) << text;
  }
}

TEST(UrlEncodePath, KeepsSlashesAndUnreserved) {
  EXPECT_EQ(url_encode_path("dir/a-b_c.~txt"), "dir/a-b_c.~txt");
  EXPECT_EQ(url_encode_path("my file#1?.txt"), "my%20file%231%3F.txt");
  EXPECT_EQ(url_encode_path("\xC3\xA9"), "%C3%A9");
}

// Refused on Linux; on WSL2 with mirrored networking the connect succeeds
// and the connection is reset. Either way: `unavailable`.
TEST(HttpClient, NobodyListeningIsUnavailable) {
  std::string port;
  {
    const SilentServer gone;
    port = gone.port();
  }
  auto client = HttpClient::create(ServerUrl{.tls = false, .host = "127.0.0.1", .port = port},
                                   {.io_timeout = 2s})
                    .value();
  const auto res = client.call(http_verb::get, "/transactions");
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code, ErrCode::unavailable);
}

TEST(HttpClient, IoTimeoutEndsAStalledRequest) {
  const SilentServer server;
  auto client = HttpClient::create(server.url(), {.io_timeout = 200ms}).value();

  const auto start = std::chrono::steady_clock::now();
  const auto res = client.call(http_verb::get, "/transactions");
  const auto took = std::chrono::steady_clock::now() - start;

  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code, ErrCode::unavailable);
  EXPECT_LT(took, 5s);
}

TEST(HttpClient, CancelFromAnotherThreadEndsARequest) {
  const SilentServer server;
  auto client = HttpClient::create(server.url()).value();  // no timeout

  std::thread canceller([&client] {
    std::this_thread::sleep_for(200ms);
    client.cancel();
  });
  const auto start = std::chrono::steady_clock::now();
  const auto res = client.call(http_verb::get, "/transactions");
  const auto took = std::chrono::steady_clock::now() - start;
  canceller.join();

  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code, ErrCode::unavailable);
  EXPECT_NE(res.error().detail.find("cancelled"), std::string::npos) << res.error().detail;
  EXPECT_LT(took, 5s);

  // Stays cancelled.
  EXPECT_EQ(client.call(http_verb::get, "/").error().code, ErrCode::unavailable);
}

TEST(HttpClient, UploadReturnsTheSourceError) {
  const SilentServer server;
  auto client = HttpClient::create(server.url(), {.io_timeout = 2s}).value();

  int calls = 0;
  BodySource source{
      .size = 100,
      .read = [&calls](std::span<std::byte> buf) -> Result<std::size_t> {
        if (++calls == 1) {
          return buf.size() < 10 ? buf.size() : 10;
        }
        return std::unexpected(Error::make(ErrCode::io, "disk on fire"));
      },
  };
  const auto res = client.upload("/x", std::move(source), Sha256Digest{Sha256Digest::Bytes{}});
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code, ErrCode::io);
  EXPECT_EQ(res.error().detail, "disk on fire");
  EXPECT_EQ(calls, 2);
}

TEST(HttpClient, UploadFailsWhenTheSourceEndsEarly) {
  const SilentServer server;
  auto client = HttpClient::create(server.url(), {.io_timeout = 2s}).value();

  BodySource source{
      .size = 100,
      .read = [](std::span<std::byte>) -> Result<std::size_t> { return 0; },
  };
  const auto res = client.upload("/x", std::move(source), Sha256Digest{Sha256Digest::Bytes{}});
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code, ErrCode::io);
}
