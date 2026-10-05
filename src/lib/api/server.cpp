#include "api/server.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>

#include <openssl/ssl.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace confide::api {

namespace {

namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using common::ErrCode;
using common::Error;

constexpr auto kTok = asio::as_tuple(asio::use_awaitable);
constexpr std::string_view kServerName = "confide";
constexpr std::string_view kTag = "api";

using Parser = http::request_parser<http::buffer_body>;

std::unexpected<Error> fail(ErrCode code, std::string detail) {
  return std::unexpected(Error::make(code, std::move(detail)));
}

std::string_view sv(beast::string_view s) {
  return {s.data(), s.size()};
}

template <class Stream>
void set_timeout(Stream& stream, std::chrono::milliseconds timeout) {
  beast::get_lowest_layer(stream).expires_after(timeout);
}

// Writes a small response outside of a handler (errors before routing).
template <class Stream>
asio::awaitable<bool> write_response(Stream& stream, const ServerOptions& options,
                                     unsigned version, bool keep_alive, Response response) {
  http::response<http::string_body> res{response.status, version};
  res.set(http::field::server, kServerName);
  if (!response.content_type.empty()) {
    res.set(http::field::content_type, response.content_type);
  }
  for (const auto& [name, value] : response.headers) {
    res.set(name, value);
  }
  res.body() = std::move(response.body);
  res.keep_alive(keep_alive);
  res.prepare_payload();

  set_timeout(stream, options.request_timeout);
  auto [ec, n] = co_await http::async_write(stream, res, kTok);
  co_return !ec;
}

// Exchange over one parsed request header. The body is read lazily through
// the same parser, so a handler decides how and whether to consume it.
template <class Stream>
class SessionExchange final : public Exchange {
public:
  SessionExchange(Stream& stream, beast::flat_buffer& buffer, Parser& parser,
                  const ServerOptions& options)
      : stream_(stream), buffer_(buffer), parser_(parser), options_(options) {}

  const RequestHeader& request() const override { return parser_.get().base(); }

  std::optional<std::uint64_t> content_length() const override {
    const auto len = parser_.content_length();
    if (!len) {
      return std::nullopt;
    }
    return *len;
  }

  asio::awaitable<Result<std::size_t>> read_body(std::span<std::byte> buf) override {
    if (close_) {
      co_return fail(ErrCode::unavailable, "connection closed");
    }
    if (!co_await send_continue()) {
      co_return fail(ErrCode::unavailable, "write of 100-continue failed");
    }
    while (!parser_.is_done()) {
      auto& body = parser_.get().body();
      body.data = buf.data();
      body.size = buf.size();

      set_timeout(stream_, options_.request_timeout);
      auto [ec, n] = co_await http::async_read(stream_, buffer_, parser_, kTok);
      if (ec == http::error::need_buffer) {
        ec = {};
      }
      if (ec == http::error::body_limit) {
        close_ = true;
        co_return fail(ErrCode::validation, "request body too large");
      }
      if (ec) {
        close_ = true;
        co_return fail(ErrCode::unavailable, "reading request body: " + ec.message());
      }
      const std::size_t got = buf.size() - body.size;
      if (got > 0) {
        co_return got;
      }
    }
    co_return std::size_t{0};
  }

  asio::awaitable<Result<std::string>> read_body_string(std::size_t max_bytes) override {
    const auto len = content_length();
    if (len && *len > max_bytes) {
      co_return fail(ErrCode::validation, "request body too large");
    }
    std::string out;
    std::vector<std::byte> buf(std::min<std::size_t>(max_bytes + 1, 64 * 1024));
    while (true) {
      auto n = co_await read_body(buf);
      if (!n) {
        co_return std::unexpected(std::move(n).error());
      }
      if (*n == 0) {
        break;
      }
      if (out.size() + *n > max_bytes) {
        close_ = true;  // the rest of the body stays unread
        co_return fail(ErrCode::validation, "request body too large");
      }
      out.append(reinterpret_cast<const char*>(buf.data()), *n);  // NOLINT
    }
    co_return out;
  }

  asio::awaitable<Result<void>> send(Response response) override {
    responded_ = true;
    if (!co_await write_response(stream_, options_, request().version(), keep_alive(),
                                 std::move(response))) {
      close_ = true;
      co_return fail(ErrCode::unavailable, "writing response failed");
    }
    co_return Result<void>{};
  }

  asio::awaitable<Result<void>> send_file(storage::Download& file,
                                          const common::Sha256Digest& sha256) override {
    responded_ = true;

    http::response<http::buffer_body> res{http::status::ok, request().version()};
    res.set(http::field::server, kServerName);
    res.set(http::field::content_type, "application/octet-stream");
    res.set(kChecksumHeader, sha256.to_hex());
    res.content_length(file.size());
    res.keep_alive(keep_alive());
    res.body().data = nullptr;
    res.body().more = true;

    http::response_serializer<http::buffer_body> sr{res};
    set_timeout(stream_, options_.request_timeout);
    if (auto [ec, n] = co_await http::async_write_header(stream_, sr, kTok); ec) {
      close_ = true;
      co_return fail(ErrCode::unavailable, "writing response header: " + ec.message());
    }

    std::vector<std::byte> buf(storage::kChunkBytes);
    std::uint64_t sent = 0;
    while (true) {
      auto n = file.read(buf);
      if (!n) {
        // Headers are out, so no error response is possible: drop the
        // connection and let the client notice the short body.
        close_ = true;
        co_return std::unexpected(std::move(n).error());
      }
      sent += *n;
      if (*n == 0 && sent != file.size()) {
        close_ = true;
        co_return fail(ErrCode::io, "file shrank while sending");
      }
      res.body().data = *n == 0 ? nullptr : buf.data();
      res.body().size = *n;
      res.body().more = *n != 0;

      set_timeout(stream_, options_.request_timeout);
      auto [ec, written] = co_await http::async_write(stream_, sr, kTok);
      if (ec == http::error::need_buffer) {
        ec = {};
      }
      if (ec) {
        close_ = true;
        co_return fail(ErrCode::unavailable, "writing response body: " + ec.message());
      }
      if (*n == 0) {
        break;
      }
    }
    co_return Result<void>{};
  }

  bool responded() const noexcept override { return responded_; }

  // True when the connection can't carry another request.
  bool must_close() const noexcept { return close_ || !keep_alive(); }

private:
  // Keep-alive only if the client wants it and the body was fully read;
  // otherwise unread body bytes would be taken for the next request.
  bool keep_alive() const { return !close_ && parser_.get().keep_alive() && parser_.is_done(); }

  // Answers "Expect: 100-continue" before the first body read, so curl
  // starts sending at once instead of waiting a second.
  asio::awaitable<bool> send_continue() {
    if (continue_sent_ || parser_.is_done()) {
      co_return true;
    }
    continue_sent_ = true;
    if (!beast::iequals(request()[http::field::expect], "100-continue")) {
      co_return true;
    }
    http::response<http::empty_body> res{http::status::continue_, request().version()};
    set_timeout(stream_, options_.request_timeout);
    auto [ec, n] = co_await http::async_write(stream_, res, kTok);
    co_return !ec;
  }

  Stream& stream_;
  beast::flat_buffer& buffer_;
  Parser& parser_;
  const ServerOptions& options_;
  bool responded_ = false;
  bool continue_sent_ = false;
  bool close_ = false;
};

}  // namespace

// --- TLS --------------------------------------------------------------------

Result<std::shared_ptr<ssl::context>> make_tls_context(const config::TlsCfg& cfg) {
  auto ctx = std::make_shared<ssl::context>(ssl::context::tls_server);
  ctx->set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                   ssl::context::no_sslv3 | ssl::context::no_tlsv1 |
                   ssl::context::no_tlsv1_1 | ssl::context::single_dh_use);

  boost::system::error_code ec;
  ctx->use_certificate_chain_file(cfg.cert_path.string(), ec);
  if (ec) {
    return fail(ErrCode::io, "TLS certificate " + cfg.cert_path.string() + ": " + ec.message());
  }
  ctx->use_private_key_file(cfg.key_path.string(), ssl::context::pem, ec);
  if (ec) {
    return fail(ErrCode::io, "TLS key " + cfg.key_path.string() + ": " + ec.message());
  }
  if (SSL_CTX_check_private_key(ctx->native_handle()) != 1) {
    return fail(ErrCode::validation, "TLS key does not match the certificate");
  }
  return ctx;
}

Result<ServerOptions> ServerOptions::from_config(const config::Config& cfg) {
  ServerOptions options{
      .bind = cfg.rest().bind,
      .port = cfg.rest().port,
      .threads = cfg.rest().threads,
      .request_timeout = std::chrono::milliseconds{cfg.rest().request_timeout_ms},
  };
  if (cfg.tls().enabled) {
    CFD_TRY(tls, make_tls_context(cfg.tls()));
    options.tls = std::move(tls);
  }
  return options;
}

// --- Server -----------------------------------------------------------------

// One open connection, shared between its coroutine and stop(). Both only
// touch `stream` on the connection's strand, so stop() never races the
// coroutine and never closes a stream that is already gone.
struct Connection {
  asio::any_io_executor strand;
  beast::tcp_stream* stream = nullptr;  // null once the coroutine has finished
};

struct HttpServer::Impl {
  Impl(ServerOptions opts, const Router& r, const IAuthenticator& a, logging::TechLog& t)
      : options(std::move(opts)), router(r), auth(a), tlog(t) {}

  asio::awaitable<void> accept_loop();
  asio::awaitable<void> serve(tcp::socket socket);

  template <class Stream>
  asio::awaitable<void> serve_requests(Stream& stream);

  template <class Stream>
  asio::awaitable<void> dispatch(SessionExchange<Stream>& ex);

  // Registers a connection for stop(). nullptr if the server is stopping:
  // the caller must then close the connection itself.
  std::shared_ptr<Connection> track(beast::tcp_stream& stream);
  void forget(const std::shared_ptr<Connection>& conn);

  // Closes the acceptor and every connection; the coroutines then finish on
  // their own and io_context::run() returns once no work is left. Nothing is
  // abandoned in a suspended frame, so ~io_context has nothing to destroy.
  void stop();

  // Declared first, destroyed last: every socket, timer and coroutine frame
  // belongs to it.
  asio::io_context ioc;
  ServerOptions options;
  const Router& router;
  const IAuthenticator& auth;
  logging::TechLog& tlog;

  std::optional<tcp::acceptor> acceptor;
  std::atomic<std::uint16_t> port{0};

  std::mutex mu;  // guards everything below
  bool started = false;
  bool stopping = false;
  std::vector<std::thread> threads;
  std::vector<std::shared_ptr<Connection>> connections;
};

std::shared_ptr<Connection> HttpServer::Impl::track(beast::tcp_stream& stream) {
  std::lock_guard lock{mu};
  if (stopping) {
    return nullptr;
  }
  auto conn = std::make_shared<Connection>(Connection{.strand = stream.get_executor(),
                                                      .stream = &stream});
  connections.push_back(conn);
  return conn;
}

void HttpServer::Impl::forget(const std::shared_ptr<Connection>& conn) {
  conn->stream = nullptr;  // on the connection's strand
  std::lock_guard lock{mu};
  std::erase(connections, conn);
}

void HttpServer::Impl::stop() {
  std::vector<std::shared_ptr<Connection>> open;
  {
    std::lock_guard lock{mu};
    if (stopping) {
      return;
    }
    stopping = true;
    open = connections;
  }
  if (acceptor) {
    asio::post(acceptor->get_executor(), [this] {
      boost::system::error_code ec;
      acceptor->close(ec);
    });
  }
  for (auto& conn : open) {
    asio::post(conn->strand, [conn] {
      if (conn->stream != nullptr) {
        conn->stream->close();
      }
    });
  }
}

// GCC false positive: the temporary produced by `co_await` lives in the coroutine frame and
// GCC cannot prove it is initialized when the frame is torn down.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
asio::awaitable<void> HttpServer::Impl::accept_loop() {
  while (acceptor->is_open()) {
    auto [ec, socket] = co_await acceptor->async_accept(asio::make_strand(ioc), kTok);
    if (ec == asio::error::operation_aborted || !acceptor->is_open()) {
      co_return;
    }
    if (ec) {
      tlog.warn(kTag, "accept failed: {}", ec.message());
      continue;
    }
    auto exec = socket.get_executor();
    asio::co_spawn(exec, serve(std::move(socket)), asio::detached);
  }
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

asio::awaitable<void> HttpServer::Impl::serve(tcp::socket socket) {
  beast::tcp_stream stream{std::move(socket)};
  const auto conn = track(stream);
  if (!conn) {
    co_return;  // stopping; the socket closes with `stream`
  }

  try {
    if (!options.tls) {
      co_await serve_requests(stream);
      boost::system::error_code ec;
      stream.socket().shutdown(tcp::socket::shutdown_send, ec);
    } else {
      // `stream` is moved into the TLS stream; track its lowest layer instead.
      ssl::stream<beast::tcp_stream> tls_stream{std::move(stream), *options.tls};
      conn->stream = &beast::get_lowest_layer(tls_stream);
      set_timeout(tls_stream, options.request_timeout);
      auto [ec] = co_await tls_stream.async_handshake(ssl::stream_base::server, kTok);
      if (ec) {
        tlog.debug(kTag, "TLS handshake failed: {}", ec.message());
      } else {
        co_await serve_requests(tls_stream);
        set_timeout(tls_stream, options.request_timeout);
        co_await tls_stream.async_shutdown(kTok);
      }
    }
  } catch (const std::exception& e) {
    tlog.error(kTag, "connection aborted: {}", e.what());
  }
  // No suspension since the stream's last use, so stop() can't see a
  // dangling pointer in between.
  forget(conn);
}

template <class Stream>
asio::awaitable<void> HttpServer::Impl::serve_requests(Stream& stream) {
  beast::flat_buffer buffer;
  while (true) {
    Parser parser;
    parser.header_limit(options.max_header_bytes);
    if (options.max_upload_bytes != 0) {
      parser.body_limit(options.max_upload_bytes);
    } else {
      parser.body_limit(boost::none);
    }

    set_timeout(stream, options.request_timeout);
    auto [ec, n] = co_await http::async_read_header(stream, buffer, parser, kTok);
    if (ec == http::error::end_of_stream || ec == asio::error::eof) {
      co_return;
    }
    if (ec == http::error::header_limit) {
      co_await write_response(stream, options, 11, false,
                              Response{
                                  .status = http::status::payload_too_large,
                                  .body = error_body(Error::make(ErrCode::validation,
                                                                 "request header too large")),
                              });
      co_return;
    }
    if (ec == beast::error::timeout || ec == asio::error::operation_aborted) {
      co_return;
    }
    if (ec) {
      // Malformed request line or headers; connection errors end here too,
      // where the write simply fails.
      tlog.debug(kTag, "bad request: {}", ec.message());
      co_await write_response(stream, options, 11, false,
                              Response::error(Error::make(ErrCode::validation, ec.message())));
      co_return;
    }

    SessionExchange<Stream> ex{stream, buffer, parser, options};
    co_await dispatch(ex);
    if (ex.must_close()) {
      co_return;
    }
  }
}

template <class Stream>
asio::awaitable<void> HttpServer::Impl::dispatch(SessionExchange<Stream>& ex) {
  const auto& req = ex.request();

  auto match = router.match(req.method(), sv(req.target()));
  if (!match) {
    auto res = Response::error(match.error());
    if (match.error().code == ErrCode::conflict) {
      res.status = http::status::method_not_allowed;
    }
    co_await ex.send(std::move(res));
    co_return;
  }

  const Route& route = *match->route;
  auto caller = route.access == Access::peer ? auth.authenticate_peer(req)
                                             : auth.authenticate_user(req);
  if (!caller) {
    auto res = Response::error(caller.error());
    if (caller.error().code == ErrCode::auth) {
      res.headers.emplace_back("WWW-Authenticate", route.access == Access::peer
                                                       ? R"(Bearer realm="confide-peer")"
                                                       : R"(Basic realm="confide")");
    }
    co_await ex.send(std::move(res));
    co_return;
  }

  RequestContext ctx{
      .exchange = ex,
      .caller = std::move(*caller),
      .params = std::move(match->params),
      .query = std::move(match->query),
  };

  std::string what;
  try {
    co_await route.handler(ctx);
  } catch (const std::exception& e) {
    what = e.what();
  }

  if (!what.empty() || !ex.responded()) {
    if (what.empty()) {
      what = "handler sent no response";
    }
    tlog.error(kTag, "{} {}: {}", sv(req.method_string()), sv(req.target()), what);
    if (!ex.responded()) {
      co_await ex.send(Response::error(Error::make(ErrCode::internal, "internal error")));
    }
  }
}

HttpServer::HttpServer(ServerOptions options, const Router& router, const IAuthenticator& auth,
                       logging::TechLog& tlog)
    : impl_(std::make_unique<Impl>(std::move(options), router, auth, tlog)) {}

HttpServer::~HttpServer() {
  stop();
  wait();
}

Result<void> HttpServer::start() {
  auto& s = *impl_;
  {
    std::lock_guard lock{s.mu};
    if (s.started || s.stopping) {
      return fail(ErrCode::internal, "server already started or stopped");
    }
  }

  boost::system::error_code ec;
  const auto address = asio::ip::make_address(s.options.bind, ec);
  if (ec) {
    return fail(ErrCode::validation, "bad bind address " + s.options.bind + ": " + ec.message());
  }
  const tcp::endpoint endpoint{address, s.options.port};

  auto& acceptor = s.acceptor.emplace(asio::make_strand(s.ioc));
  const auto where = s.options.bind + ":" + std::to_string(s.options.port);
  if (acceptor.open(endpoint.protocol(), ec); ec) {
    return fail(ErrCode::io, "open " + where + ": " + ec.message());
  }
  if (acceptor.set_option(asio::socket_base::reuse_address(true), ec); ec) {
    return fail(ErrCode::io, "reuse_address " + where + ": " + ec.message());
  }
  if (acceptor.bind(endpoint, ec); ec) {
    return fail(ErrCode::io, "bind " + where + ": " + ec.message());
  }
  if (acceptor.listen(asio::socket_base::max_listen_connections, ec); ec) {
    return fail(ErrCode::io, "listen " + where + ": " + ec.message());
  }
  s.port = acceptor.local_endpoint().port();

  asio::co_spawn(acceptor.get_executor(), s.accept_loop(), asio::detached);

  auto count = s.options.threads != 0 ? s.options.threads : std::thread::hardware_concurrency();
  count = std::max(count, 1U);
  {
    std::lock_guard lock{s.mu};
    s.started = true;
    for (unsigned i = 0; i < count; ++i) {
      s.threads.emplace_back([&s] { s.ioc.run(); });
    }
  }

  s.tlog.info(kTag, "listening on {}:{} ({}, {} threads)", s.options.bind, s.port.load(),
              s.options.tls ? "https" : "http", count);
  return {};
}

void HttpServer::stop() {
  impl_->stop();
}

void HttpServer::wait() {
  std::vector<std::thread> threads;
  {
    std::lock_guard lock{impl_->mu};
    threads.swap(impl_->threads);
  }
  for (auto& t : threads) {
    if (t.joinable()) {
      t.join();
    }
  }
}

std::uint16_t HttpServer::port() const noexcept {
  return impl_->port.load();
}

}  // namespace confide::api
