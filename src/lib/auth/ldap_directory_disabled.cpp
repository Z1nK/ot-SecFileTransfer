#include "auth/ldap_directory.hpp"

// Built when WITH_LDAP is OFF: the config may still say provider = "ldap",
// which then fails at startup with a clear message.

namespace confide::auth {

Result<std::unique_ptr<IUserDirectory>> make_ldap_directory(const config::LdapCfg& /*cfg*/,
                                                            logging::TechLog* /*log*/) {
  return std::unexpected(common::Error::make(
      common::ErrCode::validation,
      "auth.provider = \"ldap\" but this build has no LDAP support (rebuild with -DWITH_LDAP=ON)"));
}

}  // namespace confide::auth
