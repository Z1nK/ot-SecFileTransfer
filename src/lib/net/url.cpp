#include "net/client/io.hpp"
#include "net/http_client.hpp"

#include <charconv>
#include <string>
#include <string_view>

namespace confide::net {

using detail::ErrCode;
using detail::fail;
using detail::kDefaultPort;

Result<ServerUrl> ServerUrl::parse(std::string_view text) {
  ServerUrl url;
  if (text.starts_with("https://")) {
    url.tls = true;
    text.remove_prefix(8);
  } else if (text.starts_with("http://")) {
    text.remove_prefix(7);
  } else {
    return fail(ErrCode::validation, "server URL must start with http:// or https://");
  }
  if (text.ends_with('/')) {
    text.remove_suffix(1);
  }
  if (text.find('/') != std::string_view::npos) {
    return fail(ErrCode::validation, "server URL must not have a path");
  }

  const auto colon = text.rfind(':');
  url.host = std::string{text.substr(0, colon)};
  url.port = colon == std::string_view::npos ? std::string{kDefaultPort}
                                             : std::string{text.substr(colon + 1)};
  if (url.host.empty()) {
    return fail(ErrCode::validation, "server URL has no host");
  }
  unsigned port = 0;
  const auto* end = url.port.data() + url.port.size();
  const auto [ptr, ec] = std::from_chars(url.port.data(), end, port);
  if (ec != std::errc{} || ptr != end || port == 0 || port > 65535) {
    return fail(ErrCode::validation, "bad port in server URL: " + url.port);
  }
  return url;
}

std::string url_encode_path(std::string_view path) {
  constexpr std::string_view kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(path.size());
  for (const char c : path) {
    const auto u = static_cast<unsigned char>(c);
    const bool keep = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9')
                      || c == '-' || c == '.' || c == '_' || c == '~' || c == '/';
    if (keep) {
      out.push_back(c);
    } else {
      out.push_back('%');
      out.push_back(kHex[u >> 4]);
      out.push_back(kHex[u & 0x0F]);
    }
  }
  return out;
}

}  // namespace confide::net
