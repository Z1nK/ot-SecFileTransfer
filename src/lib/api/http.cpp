#include "api/http.hpp"

#include <boost/json.hpp>

#include <utility>

namespace confide::api {

using common::ErrCode;

http::status status_for(ErrCode code) {
  switch (code) {
  case ErrCode::validation:
    return http::status::bad_request;
  case ErrCode::auth:
    return http::status::unauthorized;
  case ErrCode::forbidden:
    return http::status::forbidden;
  case ErrCode::notfound:
    return http::status::not_found;
  case ErrCode::conflict:
    return http::status::conflict;
  case ErrCode::gone:
    return http::status::gone;
  case ErrCode::integrity:
  case ErrCode::unknown_destination:
    return http::status::unprocessable_entity;
  case ErrCode::unavailable:
    return http::status::service_unavailable;
  case ErrCode::io:
  case ErrCode::internal:
    return http::status::internal_server_error;
  }
  return http::status::internal_server_error;
}

std::string error_body(const common::Error& err) {
  return boost::json::serialize(boost::json::object{
      {"error", common::to_string(err.code)},
      {"detail", err.detail},
  });
}

Response Response::json(http::status status, std::string body) {
  return Response{.status = status, .body = std::move(body)};
}

Response Response::error(const common::Error& err) {
  return Response{.status = status_for(err.code), .body = error_body(err)};
}

Response Response::no_content() {
  return Response{.status = http::status::no_content, .content_type = {}};
}

}  // namespace confide::api
