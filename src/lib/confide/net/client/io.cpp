#include "net/client/io.hpp"

#include <boost/json.hpp>

#include <format>

namespace confide::net::detail {

namespace {

namespace json = boost::json;

// Fallback when the error body does not name a known code.
ErrCode code_for(http::status status) {
  switch (status) {
  case http::status::bad_request:
    return ErrCode::validation;
  case http::status::unauthorized:
    return ErrCode::auth;
  case http::status::forbidden:
    return ErrCode::forbidden;
  case http::status::not_found:
    return ErrCode::notfound;
  case http::status::method_not_allowed:
  case http::status::conflict:
    return ErrCode::conflict;
  case http::status::gone:
    return ErrCode::gone;
  case http::status::unprocessable_entity:
    return ErrCode::integrity;
  case http::status::service_unavailable:
    return ErrCode::unavailable;
  case http::status::too_many_requests:
    return ErrCode::throttled;
  default:
    return ErrCode::internal;
  }
}

}  // namespace

std::unexpected<Error> net_error(std::string_view what, const beast::error_code& ec) {
  return fail(ErrCode::unavailable, std::format("{}: {}", what, ec.message()));
}

Error server_error(http::status status, std::string_view body) {
  ErrCode code = code_for(status);
  std::string detail = std::format("HTTP {}", static_cast<unsigned>(status));

  boost::system::error_code ec;
  const json::value v = json::parse(body, ec);
  if (ec || !v.is_object()) {
    return Error::make(code, std::move(detail));
  }
  const auto& obj = v.get_object();
  if (const auto* name = obj.if_contains("error"); name != nullptr && name->is_string()) {
    for (auto c = ErrCode::validation; c <= ErrCode::internal;
         c = static_cast<ErrCode>(static_cast<int>(c) + 1)) {
      if (common::to_string(c) == name->get_string()) {
        code = c;
        break;
      }
    }
  }
  if (const auto* text = obj.if_contains("detail"); text != nullptr && text->is_string()) {
    detail = std::string{text->get_string()};
  }
  return Error::make(code, std::move(detail));
}

}  // namespace confide::net::detail
