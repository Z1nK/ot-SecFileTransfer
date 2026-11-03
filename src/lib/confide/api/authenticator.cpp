#include "api/authenticator.hpp"

namespace confide::api {

namespace {

std::unexpected<common::Error> no_admins() {
  return std::unexpected(common::Error::make(common::ErrCode::forbidden, "admin rights required"));
}

}  // namespace

Result<transaction::Caller> IAuthenticator::authenticate_admin(const RequestHeader& /*req*/,
                                                               std::string_view /*source*/) const {
  return no_admins();
}

Result<transaction::Caller> IAuthenticator::authenticate_admin_password(
    const RequestHeader& /*req*/, std::string_view /*source*/) const {
  return no_admins();
}

}  // namespace confide::api
