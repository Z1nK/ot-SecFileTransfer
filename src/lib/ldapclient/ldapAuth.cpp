#include "ldapclient/ldapAuth.hpp"

#include "ldapclient/connection.hpp"

#include <memory>
#include <utility>

namespace ldapauth {

using detail::Connection;
using detail::Entry;
using detail::escapeFilterValue;
using detail::sameDn;
using detail::Scope;

namespace {

constexpr int kInvalidCredentials = 49; // LDAP_INVALID_CREDENTIALS
constexpr int kOther = 80;              // LDAP_OTHER

// Fill {username} / {dn} placeholders with filter-escaped values, so input
// like "*" or ")(uid=*" can't change the meaning of the filter.
// Single left-to-right pass over the template: inserted values are never
// scanned again, so a username containing "{dn}" stays as typed.
std::string expand(const std::string &filter, const std::string &username,
                   const std::string &dn = {}) {
  const std::pair<std::string, std::string> placeholders[] = {
      {"{username}", escapeFilterValue(username)},
      {"{dn}", escapeFilterValue(dn)},
  };
  std::string out;
  out.reserve(filter.size());
  for (std::size_t i = 0; i < filter.size();) {
    bool replaced = false;
    for (const auto &[name, value] : placeholders) {
      if (filter.compare(i, name.size(), name) == 0) {
        out += value;
        i += name.size();
        replaced = true;
        break;
      }
    }
    if (!replaced)
      out += filter[i++];
  }
  return out;
}

bool equalsIgnoreCase(const std::string &a, const std::string &b) {
  CaseInsensitiveLess less;
  return !less(a, b) && !less(b, a);
}

std::unique_ptr<Connection> serviceConnection(const Config &c) {
  auto conn = std::make_unique<Connection>(c);
  conn->bind(c.bindDn, c.bindPassword);
  return conn;
}

std::vector<std::string> userAttributes(const Config &c) {
  std::vector<std::string> attrs = {c.usernameAttribute,
                                    c.displayNameAttribute, c.mailAttribute};
  attrs.insert(attrs.end(), c.extraUserAttributes.begin(),
               c.extraUserAttributes.end());
  return attrs;
}

User toUser(const Config &c, const Entry &e, const std::string &login) {
  User u;
  u.dn = e.dn;
  u.username = e.first(c.usernameAttribute);
  if (u.username.empty())
    u.username = login;
  u.displayName = e.first(c.displayNameAttribute);
  u.mail = e.first(c.mailAttribute);
  u.attributes = e.attributes;
  return u;
}

Group toGroup(const Config &c, const Entry &e) {
  Group g;
  g.dn = e.dn;
  g.name = e.first(c.groupNameAttribute);
  g.description = e.first("description");
  if (auto it = e.attributes.find(c.memberAttribute);
      it != e.attributes.end())
    g.members = it->second;
  return g;
}

std::vector<Group> searchGroups(const Config &c, Connection &conn,
                                const std::string &filter, bool withMembers) {
  std::vector<std::string> attrs = {c.groupNameAttribute, "description"};
  if (withMembers)
    attrs.push_back(c.memberAttribute);
  std::vector<Group> groups;
  for (const auto &e : conn.search(c.groupBaseDn, Scope::Subtree, filter, attrs))
    groups.push_back(toGroup(c, e));
  return groups;
}

std::vector<Group> groupsOfUser(const Config &c, Connection &conn,
                                const User &u) {
  if (c.groupBaseDn.empty())
    return {};
  return searchGroups(c, conn, expand(c.memberOfFilter, u.username, u.dn),
                      /*withMembers=*/false);
}

void fillGroups(const Config &c, Connection &conn, User &u) {
  for (const auto &g : groupsOfUser(c, conn, u)) {
    u.groups.push_back(g.name);
    u.groupDns.push_back(g.dn);
  }
}

} // namespace

std::string User::first(const std::string &attribute) const {
  auto it = attributes.find(attribute);
  if (it == attributes.end() || it->second.empty())
    return {};
  return it->second.front();
}

bool User::inGroup(const std::string &groupName) const {
  for (const auto &g : groups)
    if (equalsIgnoreCase(g, groupName))
      return true;
  return false;
}

bool User::inGroupDn(const std::string &groupDn) const {
  for (const auto &dn : groupDns)
    if (sameDn(dn, groupDn))
      return true;
  return false;
}

Config Config::activeDirectory() {
  Config c;
  c.userFilter = "(&(objectCategory=person)(objectClass=user)"
                 "(sAMAccountName={username}))";
  c.usernameAttribute = "sAMAccountName";
  c.displayNameAttribute = "displayName";
  c.mailAttribute = "mail";
  c.groupFilter = "(objectClass=group)";
  // LDAP_MATCHING_RULE_IN_CHAIN: also matches groups the user is in through
  // nested groups.
  c.memberOfFilter =
      "(&(objectClass=group)(member:1.2.840.113556.1.4.1941:={dn}))";
  // Unique per domain, unlike cn which is only unique per OU.
  c.groupNameAttribute = "sAMAccountName";
  c.memberAttribute = "member";
  return c;
}

const char *toString(AuthStatus status) {
  switch (status) {
  case AuthStatus::Success:
    return "success";
  case AuthStatus::InvalidCredentials:
    return "invalid credentials";
  case AuthStatus::UserNotFound:
    return "user not found";
  case AuthStatus::AmbiguousUser:
    return "ambiguous user";
  }
  return "unknown";
}

LdapAuth::LdapAuth(Config config) : config_(std::move(config)) {}

AuthResult LdapAuth::authenticate(const std::string &username,
                                  const std::string &password) const {
  AuthResult result;
  if (username.empty()) {
    result.status = AuthStatus::UserNotFound;
    result.message = "empty username";
    return result;
  }
  if (password.empty()) {
    // Never send this to the server: DN + empty password is an
    // "unauthenticated bind", which succeeds on many servers.
    result.status = AuthStatus::InvalidCredentials;
    result.message = "empty password";
    return result;
  }

  // 1. Find the user's DN with the service account.
  auto svc = serviceConnection(config_);
  auto entries = svc->search(config_.userBaseDn, Scope::Subtree,
                             expand(config_.userFilter, username),
                             userAttributes(config_));
  if (entries.size() != 1) {
    result.status = entries.empty() ? AuthStatus::UserNotFound
                                    : AuthStatus::AmbiguousUser;
    result.message = std::to_string(entries.size()) +
                     " entries match user '" + username + "'";
    return result;
  }

  // 2. Verify the password: bind as that DN on a separate connection.
  try {
    Connection userConn(config_);
    userConn.bind(entries.front().dn, password);
  } catch (const LdapError &e) {
    if (e.code() != kInvalidCredentials)
      throw;
    result.status = AuthStatus::InvalidCredentials;
    result.message = e.what();
    return result;
  }

  // 3. Group membership, read with the service account (users often may not
  // read groups themselves).
  User user = toUser(config_, entries.front(), username);
  fillGroups(config_, *svc, user);

  result.status = AuthStatus::Success;
  result.user = std::move(user);
  return result;
}

std::optional<User> LdapAuth::findUser(const std::string &username) const {
  if (username.empty())
    return std::nullopt;
  auto svc = serviceConnection(config_);
  auto entries = svc->search(config_.userBaseDn, Scope::Subtree,
                             expand(config_.userFilter, username),
                             userAttributes(config_));
  if (entries.size() != 1)
    return std::nullopt;
  User user = toUser(config_, entries.front(), username);
  fillGroups(config_, *svc, user);
  return user;
}

std::vector<Group> LdapAuth::groupsOf(const User &user) const {
  auto svc = serviceConnection(config_);
  return groupsOfUser(config_, *svc, user);
}

std::vector<Group> LdapAuth::listGroups() const {
  if (config_.groupBaseDn.empty())
    return {};
  auto svc = serviceConnection(config_);
  return searchGroups(config_, *svc, config_.groupFilter,
                      /*withMembers=*/false);
}

std::optional<Group> LdapAuth::findGroup(const std::string &name) const {
  if (config_.groupBaseDn.empty() || name.empty())
    return std::nullopt;
  auto svc = serviceConnection(config_);
  std::string filter = "(&" + config_.groupFilter + "(" +
                       config_.groupNameAttribute + "=" +
                       escapeFilterValue(name) + "))";
  auto groups = searchGroups(config_, *svc, filter, /*withMembers=*/true);
  if (groups.empty())
    return std::nullopt;
  if (groups.size() > 1) {
    // Picking one would be a guess: the server returns them in no
    // particular order.
    std::string what = "findGroup(" + name + "): " +
                       std::to_string(groups.size()) + " groups match:";
    for (const auto &g : groups)
      what += " [" + g.dn + "]";
    throw LdapError(what, kOther);
  }
  return groups.front();
}

} // namespace ldapauth
