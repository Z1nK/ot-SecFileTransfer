#pragma once
#include "auth/user_directory.hpp"
#include "config/config.hpp"
#include "logging/tech_log.hpp"

#include <memory>

namespace confide::auth {

// Internal to auth: only user_directory.cpp calls this. Two definitions,
// CMake compiles exactly one: ldap_directory.cpp (WITH_LDAP) or
// ldap_directory_disabled.cpp, which returns `validation`.
Result<std::unique_ptr<IUserDirectory>> make_ldap_directory(const config::LdapCfg& cfg,
                                                            logging::TechLog* log);

}  // namespace confide::auth
