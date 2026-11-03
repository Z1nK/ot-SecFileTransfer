#pragma once
// Internal to HttpClient: the request body behind upload(target, BodySource).
#include "net/client/io.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/optional.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace confide::net::detail {

// Beast body that pulls its bytes from a BodySource. An error from the
// source is kept in `failure`, since Beast only passes an error_code on.
struct SourceBody {
  struct value_type {
    BodySource source;
    std::optional<Error> failure{};
  };

  static std::uint64_t size(const value_type& body) { return body.source.size; }

  class writer {
  public:
    using const_buffers_type = asio::const_buffer;

    template <bool isRequest, class Fields>
    writer(http::header<isRequest, Fields>& /*header*/, value_type& body)
        : body_(body)
        , left_(body.source.size)
        , buf_(static_cast<std::size_t>(std::min<std::uint64_t>(left_, kSourceChunkBytes))) {}

    void init(beast::error_code& ec) { ec = {}; }

    boost::optional<std::pair<const_buffers_type, bool>> get(beast::error_code& ec) {
      ec = {};
      if (left_ == 0) {
        return boost::none;
      }
      const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(left_, buf_.size()));
      auto n = body_.source.read(std::span{buf_.data(), want});
      if (!n || *n == 0 || *n > want) {
        body_.failure =
            n ? Error::make(ErrCode::io, std::format("body source ended {} bytes early", left_))
              : std::move(n).error();
        ec = beast::errc::make_error_code(beast::errc::io_error);
        return boost::none;
      }
      left_ -= *n;
      return std::pair{asio::const_buffer(buf_.data(), *n), left_ > 0};
    }

  private:
    value_type& body_;
    std::uint64_t left_;
    std::vector<std::byte> buf_;
  };
};

}  // namespace confide::net::detail
