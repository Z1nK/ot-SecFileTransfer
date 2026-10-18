#pragma once

// ldapauth: small read-only library to authenticate users against an LDAP
// directory (OpenLDAP, Active Directory, ...) and read their group
// memberships. It never writes to the directory.

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ldapauth {

// Thrown when the directory cannot be used at all: server unreachable, TLS
// failure, service account bind rejected, bad base DN or filter, a group name
// matching several groups, ...
// A wrong user password is NOT an exception; see AuthResult.
// code() is the raw LDAP result code (e.g. -1 = server down, 32 = no such
// object, 49 = invalid credentials).
class LdapError : public std::runtime_error {
public:
  LdapError(const std::string &what, int code)
      : std::runtime_error(what), code_(code) {}
  int code() const noexcept { return code_; }

private:
  int code_;
};

// Attribute names are case-insensitive in LDAP (AD returns "displayName",
// you may ask for "displayname"), so lookups ignore case.
struct CaseInsensitiveLess {
  bool operator()(const std::string &a, const std::string &b) const;
};
using Attributes =
    std::map<std::string, std::vector<std::string>, CaseInsensitiveLess>;

struct User {
  std::string dn;
  std::string username; // value of Config::usernameAttribute
  std::string displayName;
  std::string mail;
  // Groups the user belongs to; empty if no groupBaseDn. Names are only
  // unique if your directory makes them so (AD sAMAccountName is unique per
  // domain, cn often is not); groupDns are always unique.
  std::vector<std::string> groups;   // group names
  std::vector<std::string> groupDns; // group DNs, same order as groups
  Attributes attributes;             // every attribute that was fetched

  // First value of an attribute, or empty string if missing.
  std::string first(const std::string &attribute) const;
  // Case-insensitive check against `groups`. Convenient, but if two groups
  // share a name in different OUs, membership in either one counts.
  bool inGroup(const std::string &groupName) const;
  // Exact check against `groupDns`; prefer this for authorization. DNs are
  // compared normalized, so "CN=Admins, OU=Groups,..." matches
  // "cn=admins,ou=groups,...".
  bool inGroupDn(const std::string &groupDn) const;
};

struct Group {
  std::string dn;
  std::string name;
  std::string description;
  std::vector<std::string> members; // member DNs; only filled by findGroup()
};

// All filters are LDAP filters with placeholders that are replaced by the
// properly escaped (RFC 4515) value:
//   {username}  the login name typed by the user
//   {dn}        the user's DN (group filters only)
struct Config {
  // --- connection ---
  // ldap://host:389 or ldaps://host:636. Several URIs separated by spaces
  // are tried in order (failover).
  std::string uri;
  bool startTls = false;  // upgrade ldap:// with StartTLS
  bool tlsVerify = true;  // false = accept any certificate (tests only!)
  std::string tlsCaFile;  // CA bundle (PEM); empty = system default
  int timeoutSeconds = 5;

  // Service account used to look users and groups up. Ideally a read-only
  // account. Empty bindDn = anonymous bind.
  std::string bindDn;
  std::string bindPassword;

  // --- users ---
  std::string userBaseDn;
  std::string userFilter = "(&(objectClass=inetOrgPerson)(uid={username}))";
  std::string usernameAttribute = "uid";
  std::string displayNameAttribute = "cn";
  std::string mailAttribute = "mail";
  std::vector<std::string> extraUserAttributes; // also fetched into attributes

  // --- groups (leave groupBaseDn empty to skip group lookups) ---
  std::string groupBaseDn;
  // Matches every group (listGroups / findGroup).
  std::string groupFilter = "(objectClass=groupOfNames)";
  // Matches the groups a user belongs to. For posixGroup use
  // "(&(objectClass=posixGroup)(memberUid={username}))".
  std::string memberOfFilter = "(&(objectClass=groupOfNames)(member={dn}))";
  std::string groupNameAttribute = "cn";
  std::string memberAttribute = "member";

  // Defaults for Microsoft Active Directory: login by sAMAccountName, groups
  // named by sAMAccountName (unique per domain) and group lookup that
  // includes nested groups. Set uri, bind and base DNs
  // afterwards.
  static Config activeDirectory();
};

enum class AuthStatus : std::uint8_t {
  Success,
  InvalidCredentials, // wrong/empty password, locked or expired account
  UserNotFound,
  AmbiguousUser, // userFilter matched more than one entry
};

struct AuthResult {
  AuthStatus status = AuthStatus::InvalidCredentials;
  std::optional<User> user; // set on Success
  // Server diagnostic for logs (e.g. AD "data 775" = account locked). Don't
  // show it to end users: say "invalid username or password" for every
  // failure so attackers can't probe which accounts exist.
  std::string message;

  explicit operator bool() const { return status == AuthStatus::Success; }
};

const char *toString(AuthStatus status);

// Thread-safe: every call opens its own short-lived connection, so one
// instance can be shared by the whole application.
class LdapAuth {
public:
  explicit LdapAuth(Config config);

  // Verifies the password by binding as the user. Returns a failed
  // AuthResult for bad credentials / unknown users; throws LdapError only
  // when the directory itself is unusable.
  AuthResult authenticate(const std::string &username,
                          const std::string &password) const;

  // Look a user up (with groups) without checking a password, e.g. after
  // SSO or to check permissions of someone already logged in.
  std::optional<User> findUser(const std::string &username) const;

  // Groups the given user belongs to (members not filled).
  std::vector<Group> groupsOf(const User &user) const;

  // All groups under groupBaseDn (members not filled).
  std::vector<Group> listGroups() const;

  // One group by name, including its member DNs. Throws LdapError (code 80)
  // if several groups have that name; narrow groupBaseDn or groupFilter, or
  // use a unique groupNameAttribute.
  std::optional<Group> findGroup(const std::string &name) const;

  const Config &config() const { return config_; }

private:
  Config config_;
};

} // namespace ldapauth
