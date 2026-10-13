#pragma once
#include "common/error.hpp"
#include "config/config.hpp"
#include "logging/tech_log.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace confide::auth {

using common::Result;

// Where users and groups live: local config (LocalDirectory) or an LDAP
// directory (built with WITH_LDAP). Exactly one is in use, chosen by
// `auth.provider`. Answers who may log in, which names are valid transaction
// targets and who receives a transaction sent to a group.
//
// Errors: `auth` for bad credentials (same detail for wrong password and
// unknown user), `unavailable` when the backend can't be reached. Details
// are sent to the client, so they never carry backend diagnostics.
//
// Thread-safe.
class IUserDirectory {
public:
  IUserDirectory() = default;
  IUserDirectory(const IUserDirectory&) = delete;
  IUserDirectory& operator=(const IUserDirectory&) = delete;
  IUserDirectory(IUserDirectory&&) = delete;
  IUserDirectory& operator=(IUserDirectory&&) = delete;
  virtual ~IUserDirectory() = default;

  // user + password -> the user's canonical name (LDAP may differ in case
  // from what was typed).
  virtual Result<std::string> verify(std::string_view user, std::string_view password) const = 0;

  virtual Result<bool> has_user(std::string_view name) const = 0;
  virtual Result<bool> has_group(std::string_view name) const = 0;

  // true if `user` is a member of group `group`. false for unknown names and
  // when the backend can't be reached.
  virtual bool is_member(std::string_view user, std::string_view group) const = 0;
};

// The directory `cfg.auth().provider` asks for. `validation` for a bad
// local user list, or for "ldap" in a build without LDAP support. `log`
// receives backend diagnostics that must not reach clients; may be null.
Result<std::unique_ptr<IUserDirectory>> make_directory(const config::Config& cfg,
                                                       logging::TechLog* log = nullptr);

}  // namespace confide::auth
