#pragma once
#include "auth/password.hpp"
#include "auth/user_directory.hpp"
#include "common/sha256.hpp"
#include "config/config.hpp"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace confide::auth {

// Users and groups from the config (`auth.users_file`, `[[user]]`,
// `auth.groups_file`, `[[group]]`), FR-5.
//
// PBKDF2 is slow by design, and Basic credentials come with every request.
// So after a successful check the user's password is remembered as a salted
// SHA-256 in memory, and the same password is accepted again without PBKDF2.
// A different password always goes through PBKDF2. Unknown users are checked
// against a dummy hash, so they take as long as known ones.
class LocalDirectory final : public IUserDirectory {
public:
  // `validation` naming the user whose password_hash can't be parsed, so a
  // bad config fails at startup rather than at the first login.
  static Result<std::unique_ptr<LocalDirectory>> create(const config::Config& cfg);

  Result<std::string> verify(std::string_view user, std::string_view password) const override;
  Result<bool> has_user(std::string_view name) const override;
  Result<bool> has_group(std::string_view name) const override;
  bool is_member(std::string_view user, std::string_view group) const override;

private:
  using Users = std::map<std::string, PasswordHash, std::less<>>;
  using Groups = std::map<std::string, std::vector<std::string>, std::less<>>;  // -> members

  LocalDirectory(Users users, Groups groups, std::optional<PasswordHash> dummy)
      : users_(std::move(users)), groups_(std::move(groups)), dummy_(std::move(dummy)) {}

  bool check_password(std::string_view user, const PasswordHash& hash,
                      std::string_view password) const;

  Users users_;
  Groups groups_;
  // Verified against for unknown users, so they cost the same as known ones.
  std::optional<PasswordHash> dummy_;

  // Last verified password of each user, as SHA-256(salt || password).
  mutable std::mutex mu_;
  mutable std::map<std::string, common::Sha256Digest, std::less<>> verified_;
};

}  // namespace confide::auth
