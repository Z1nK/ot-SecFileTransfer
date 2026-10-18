#include "api/server/server_impl.hpp"

#include <boost/asio/post.hpp>

#include <memory>
#include <mutex>
#include <vector>

namespace confide::api {

using detail::Connection;

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

}  // namespace confide::api
