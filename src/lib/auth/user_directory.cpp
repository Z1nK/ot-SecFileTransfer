#include "auth/user_directory.hpp"

#include "auth/ldap_directory.hpp"
#include "auth/local_directory.hpp"

namespace confide::auth {

Result<std::unique_ptr<IUserDirectory>> make_directory(const config::Config& cfg,
                                                       logging::TechLog* log) {
  switch (cfg.auth().provider) {
  case config::AuthProvider::local: {
    CFD_TRY(local, LocalDirectory::create(cfg));
    return std::unique_ptr<IUserDirectory>(std::move(local));
  }
  case config::AuthProvider::ldap:
    return make_ldap_directory(cfg.auth().ldap, log);
  }
  return std::unexpected(
      common::Error::make(common::ErrCode::internal, "unknown auth.provider"));
}

}  // namespace confide::auth
