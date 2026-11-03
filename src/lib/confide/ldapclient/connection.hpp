#pragma once

// Internal read-only wrapper over libldap. Not part of the public API.

#include "ldapclient/ldapAuth.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct ldap;

namespace ldapauth::detail {

struct Entry {
  std::string dn;
  Attributes attributes;

  std::string first(const std::string &attribute) const;
};

enum class Scope : std::uint8_t { Base, Subtree };

class Connection {
public:
  explicit Connection(const Config &config);
  ~Connection();

  Connection(const Connection &) = delete;
  Connection &operator=(const Connection &) = delete;
  Connection(Connection &&) = delete;
  Connection &operator=(Connection &&) = delete;

  // Simple bind. Throws LdapError (code 49 for bad credentials). A non-empty
  // DN with an empty password is rejected locally: servers treat it as an
  // "unauthenticated bind" that succeeds without checking anything.
  void bind(const std::string &dn, const std::string &password);

  // Search using the paged results control, so directories with a server
  // size limit (AD: 1000 entries) still return everything.
  std::vector<Entry> search(const std::string &baseDn, Scope scope,
                            const std::string &filter,
                            const std::vector<std::string> &attributes);

private:
  [[noreturn]] void fail(int rc, const std::string &context);

  ldap *ld_ = nullptr;
};

// RFC 4515 escaping for values placed into a search filter.
std::string escapeFilterValue(const std::string &value);

// True if both DNs name the same entry: ignores spacing, case and
// equivalent escapings. Falls back to a case-insensitive string compare if
// either DN can't be parsed.
bool sameDn(const std::string &a, const std::string &b);

} // namespace ldapauth::detail
