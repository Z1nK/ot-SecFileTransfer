#pragma once
#include "api/http.hpp"
#include "transaction/service.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace confide::api {

// Decodes %XX escapes exactly once ('+' stays '+'). `validation` on a
// malformed escape or an encoded NUL. Path parameters and query values are
// passed through it before they reach RelativePath::parse() (FR-6).
Result<std::string> url_decode(std::string_view text);

// Who may call a route; checked by the server before the handler runs.
enum class Access : std::uint8_t {
  user,  // a local user (Basic auth or token)
  peer,  // a peer server with its peer token (/internal/*)
};

// URL-decoded values captured by a route pattern, by name.
using PathParams = std::unordered_map<std::string, std::string>;

// URL-decoded query parameters; a repeated key keeps its last value.
using QueryParams = std::unordered_map<std::string, std::string>;

// Everything a handler gets for one request.
struct RequestContext {
  Exchange& exchange;
  transaction::Caller caller;
  PathParams params;
  QueryParams query;

  // Path parameter by name, nullptr if the route has no such parameter.
  const std::string* param(std::string_view name) const;

  // Query value, nullopt if missing.
  std::optional<std::string_view> query_value(std::string_view key) const;
};

using Handler = std::function<asio::awaitable<void>(RequestContext&)>;

struct Route {
  http::verb method;
  std::string pattern;
  Access access;
  Handler handler;
};

// Maps method + path to a handler. Patterns are '/'-separated:
//
//   "/transactions"                       literal segments
//   "/transactions/{id}"                  {name} matches one segment
//   "/transactions/{id}/files/{path*}"    {name*} matches the rest, '/' included;
//                                         only allowed as the last segment
//
// Routes are added once at startup, then only read, so match() is safe to
// call from every server thread.
class Router {
public:
  // `internal` on a malformed pattern or a duplicate method + pattern.
  Result<void> add(Route route);

  struct Match {
    const Route* route = nullptr;
    PathParams params;
    QueryParams query;
  };

  // Splits `target` (request-target, path + optional "?query") and finds a
  // route:
  //   `notfound`   - no pattern matches the path               -> 404
  //   `conflict`   - the path matches, but not for this method -> 405
  //   `validation` - bad %-escape in the path or query          -> 400
  Result<Match> match(http::verb method, std::string_view target) const;

private:
  struct Segment {
    std::string text;  // literal text, or the parameter name
    bool is_param = false;
    bool is_rest = false;
  };

  struct Entry {
    Route route;
    std::vector<Segment> segments;
  };

  // Params if the decoded `path` segments fit `pattern`, else nullopt.
  static std::optional<PathParams> match_segments(const std::vector<Segment>& pattern,
                                                  const std::vector<std::string>& path);

  std::vector<Entry> entries_;
};

}  // namespace confide::api
