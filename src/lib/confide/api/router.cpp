#include "api/router.hpp"

#include <algorithm>
#include <utility>

namespace confide::api {

namespace {

using common::ErrCode;
using common::Error;

std::unexpected<Error> fail(ErrCode code, std::string detail) {
  return std::unexpected(Error::make(code, std::move(detail)));
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// Splits "/a/b/c" into {"a", "b", "c"}; "/" gives no segments. `text` must
// start with '/'.
std::vector<std::string_view> split_path(std::string_view text) {
  std::vector<std::string_view> out;
  if (text == "/") {
    return out;
  }
  text.remove_prefix(1);
  while (true) {
    const auto pos = text.find('/');
    out.push_back(text.substr(0, pos));
    if (pos == std::string_view::npos) {
      break;
    }
    text.remove_prefix(pos + 1);
  }
  return out;
}

Result<QueryParams> parse_query(std::string_view text) {
  QueryParams out;
  while (!text.empty()) {
    const auto amp = text.find('&');
    const auto pair = text.substr(0, amp);
    text = amp == std::string_view::npos ? std::string_view{} : text.substr(amp + 1);
    if (pair.empty()) {
      continue;
    }
    const auto eq = pair.find('=');
    CFD_TRY(key, url_decode(pair.substr(0, eq)));
    CFD_TRY(value, url_decode(eq == std::string_view::npos ? std::string_view{}
                                                           : pair.substr(eq + 1)));
    out.insert_or_assign(std::move(key), std::move(value));
  }
  return out;
}

}  // namespace

Result<std::string> url_decode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%') {
      out.push_back(text[i]);
      continue;
    }
    if (i + 2 >= text.size()) {
      return fail(ErrCode::validation, "truncated %-escape");
    }
    const int hi = hex_value(text[i + 1]);
    const int lo = hex_value(text[i + 2]);
    if (hi < 0 || lo < 0) {
      return fail(ErrCode::validation, "bad %-escape");
    }
    const auto c = static_cast<char>((hi * 16) + lo);
    if (c == '\0') {
      return fail(ErrCode::validation, "encoded NUL");
    }
    out.push_back(c);
    i += 2;
  }
  return out;
}

const std::string* RequestContext::param(std::string_view name) const {
  const auto it = params.find(std::string{name});
  return it == params.end() ? nullptr : &it->second;
}

std::optional<std::string_view> RequestContext::query_value(std::string_view key) const {
  const auto it = query.find(std::string{key});
  if (it == query.end()) {
    return std::nullopt;
  }
  return std::string_view{it->second};
}

Result<void> Router::add(Route route) {
  if (route.pattern.empty() || route.pattern.front() != '/') {
    return fail(ErrCode::internal, "route pattern must start with '/': " + route.pattern);
  }
  if (!route.handler) {
    return fail(ErrCode::internal, "route without handler: " + route.pattern);
  }

  std::vector<Segment> segments;
  const auto parts = split_path(route.pattern);
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const auto part = parts[i];
    if (part.empty()) {
      return fail(ErrCode::internal, "empty segment in route pattern: " + route.pattern);
    }
    if (part.front() != '{') {
      segments.push_back({.text = std::string{part}});
      continue;
    }
    if (part.size() < 3 || part.back() != '}') {
      return fail(ErrCode::internal, "bad parameter in route pattern: " + route.pattern);
    }
    auto name = part.substr(1, part.size() - 2);
    const bool rest = name.back() == '*';
    if (rest) {
      name.remove_suffix(1);
      if (name.empty() || i + 1 != parts.size()) {
        return fail(ErrCode::internal, "{name*} must be the last segment: " + route.pattern);
      }
    }
    const bool dup = std::ranges::any_of(
        segments, [&](const Segment& s) { return s.is_param && s.text == name; });
    if (dup) {
      return fail(ErrCode::internal, "duplicate parameter in route pattern: " + route.pattern);
    }
    segments.push_back({.text = std::string{name}, .is_param = true, .is_rest = rest});
  }

  const bool exists = std::ranges::any_of(entries_, [&](const Entry& e) {
    return e.route.method == route.method && e.route.pattern == route.pattern;
  });
  if (exists) {
    return fail(ErrCode::internal, "duplicate route: " + route.pattern);
  }

  entries_.push_back({.route = std::move(route), .segments = std::move(segments)});
  return {};
}

std::optional<PathParams> Router::match_segments(const std::vector<Segment>& pattern,
                                                const std::vector<std::string>& path) {
  PathParams params;
  std::size_t i = 0;
  for (const auto& seg : pattern) {
    // A parameter never captures an empty segment ("/transactions/").
    if (i >= path.size() || (seg.is_param && path[i].empty())) {
      return std::nullopt;
    }
    if (seg.is_rest) {
      std::string rest = path[i];
      for (++i; i < path.size(); ++i) {
        rest += '/';
        rest += path[i];
      }
      params.emplace(seg.text, std::move(rest));
      return params;
    }
    if (seg.is_param) {
      params.emplace(seg.text, path[i]);
    } else if (seg.text != path[i]) {
      return std::nullopt;
    }
    ++i;
  }
  if (i != path.size()) {
    return std::nullopt;
  }
  return params;
}

Result<Router::Match> Router::match(http::verb method, std::string_view target) const {
  const auto qpos = target.find('?');
  const auto path = target.substr(0, qpos);
  if (path.empty() || path.front() != '/') {
    return fail(ErrCode::validation, "request target must start with '/'");
  }

  // Decoded once here; a "%2F" inside a segment does not split it.
  std::vector<std::string> segments;
  for (const auto raw : split_path(path)) {
    CFD_TRY(s, url_decode(raw));
    segments.push_back(std::move(s));
  }

  bool path_matched = false;
  for (const auto& entry : entries_) {
    auto params = match_segments(entry.segments, segments);
    if (!params) {
      continue;
    }
    path_matched = true;
    if (entry.route.method != method) {
      continue;
    }

    CFD_TRY(query, parse_query(qpos == std::string_view::npos ? std::string_view{}
                                                              : target.substr(qpos + 1)));
    return Match{.route = &entry.route, .params = std::move(*params), .query = std::move(query)};
  }

  if (path_matched) {
    return fail(ErrCode::conflict, "method not allowed");
  }
  return fail(ErrCode::notfound, "no such endpoint");
}

}  // namespace confide::api
