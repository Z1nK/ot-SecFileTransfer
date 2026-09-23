// Usage example for common/error.hpp: Result<T>, Error::make, CFD_TRY and
// CFD_TRYV. Simulates loading a tiny "host:port" config through a chain of
// functions, each of which can fail with a different ErrCode.

#include <common/error.hpp>

#include <charconv>
#include <cstdint>
#include <format>
#include <iostream>
#include <map>
#include <string>
#include <string_view>

using confide::common::ErrCode;
using confide::common::Error;
using confide::common::Result;

namespace {

// Stand-in for the filesystem so the example is self-contained.
const std::map<std::string, std::string, std::less<>> kFiles = {
    {"good.cfg", "example.org:8443"},
    {"bad-port.cfg", "example.org:99999"},
    {"no-colon.cfg", "example.org"},
};

// Leaf function: produces a value or an Error. `origin` is captured here
// automatically via the default std::source_location argument.
Result<std::string> read_file(std::string_view name) {
  auto it = kFiles.find(name);
  if (it == kFiles.end()) {
    return std::unexpected(Error::make(ErrCode::io, std::format("cannot open '{}'", name)));
  }
  return it->second;
}

Result<std::uint16_t> parse_port(std::string_view text) {
  std::uint16_t port = 0;
  auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), port);
  if (ec != std::errc{} || ptr != text.data() + text.size() || port == 0) {
    return std::unexpected(Error::make(ErrCode::validation, std::format("bad port '{}'", text)));
  }
  return port;
}

// Result<void>: success carries no value, only "ok or Error".
Result<void> check_host(std::string_view host) {
  if (host.empty()) {
    return std::unexpected(Error::make(ErrCode::validation, "empty host"));
  }
  return {};
}

struct Endpoint {
  std::string host;
  std::uint16_t port;
};

// Mid-level function: CFD_TRY unwraps a Result or returns its Error to the
// caller unchanged (including the original `origin`), like Rust's `?`.
Result<Endpoint> load_endpoint(std::string_view file) {
  CFD_TRY(text, read_file(file));  // text is std::string

  auto colon = text.rfind(':');
  if (colon == std::string::npos) {
    return std::unexpected(Error::make(ErrCode::validation, "expected 'host:port'"));
  }
  std::string host = text.substr(0, colon);

  CFD_TRYV(check_host(host));                                           // Result<void>
  CFD_TRY(port, parse_port(std::string_view{text}.substr(colon + 1)));  // std::uint16_t

  return Endpoint{std::move(host), port};
}

// Same pipeline written with std::expected's monadic API instead of macros,
// for comparison. Handy for short chains; CFD_TRY reads better for long ones.
Result<std::uint16_t> load_port_only(std::string_view file) {
  return read_file(file).and_then([](const std::string& text) -> Result<std::uint16_t> {
    auto colon = text.rfind(':');
    if (colon == std::string::npos) {
      return std::unexpected(Error::make(ErrCode::validation, "expected 'host:port'"));
    }
    return parse_port(std::string_view{text}.substr(colon + 1));
  });
}

void report(const Error& err) {
  std::cout << std::format("  error [{}]: {}\n    at {}:{} ({})\n", to_string(err.code), err.detail,
                           err.origin.file_name(), err.origin.line(), err.origin.function_name());
}

}  // namespace

int main() {
  for (std::string_view file : {"good.cfg", "bad-port.cfg", "no-colon.cfg", "missing.cfg"}) {
    std::cout << file << ":\n";

    // Top level: this is where errors stop propagating and get handled.
    if (auto ep = load_endpoint(file)) {
      std::cout << std::format("  ok: host={} port={}\n", ep->host, ep->port);
    } else {
      report(ep.error());
    }

    // value_or: fall back to a default instead of handling the error.
    std::cout << std::format("  port_or_default={}\n", load_port_only(file).value_or(443));
  }
  return 0;
}
