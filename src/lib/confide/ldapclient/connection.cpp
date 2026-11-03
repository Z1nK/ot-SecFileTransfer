#include "ldapclient/connection.hpp"

#include "ldapclient/ldapAuth.hpp"

#include <lber.h>
#include <lber_types.h>
#include <ldap.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <format>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ldapauth {

bool CaseInsensitiveLess::operator()(const std::string &a,
                                     const std::string &b) const {
  const auto lower = [](char c) {
    return std::tolower(static_cast<unsigned char>(c));
  };
  return std::ranges::lexicographical_compare(a, b, {}, lower, lower);
}

namespace detail {

namespace {

constexpr ber_int_t kPageSize = 500;

using MessagePtr = std::unique_ptr<LDAPMessage, decltype(&ldap_msgfree)>;

int toLdapScope(Scope scope) {
  return scope == Scope::Base ? LDAP_SCOPE_BASE : LDAP_SCOPE_SUBTREE;
}

Entry readEntry(LDAP *ld, LDAPMessage *e) {
  Entry entry;
  if (char *dn = ldap_get_dn(ld, e)) {
    entry.dn = dn;
    ldap_memfree(dn);
  }
  BerElement *ber = nullptr;
  for (char *attr = ldap_first_attribute(ld, e, &ber); attr;
       attr = ldap_next_attribute(ld, e, ber)) {
    auto &values = entry.attributes[attr];
    if (berval **vals = ldap_get_values_len(ld, e, attr)) {
      const auto count = static_cast<std::size_t>(ldap_count_values_len(vals));
      values.reserve(count);
      for (const berval *v : std::span(vals, count)) {
        values.emplace_back(v->bv_val, v->bv_len);
      }
      ldap_value_free_len(vals);
    }
    ldap_memfree(attr);
  }
  if (ber) {
    ber_free(ber, 0);
  }
  return entry;
}

// Cookie for the next page from the paged results response control; empty
// when this was the last page.
std::string nextPageCookie(LDAP *ld, LDAPControl **resCtrls) {
  std::string cookie;
  LDAPControl *c =
      ldap_control_find(LDAP_CONTROL_PAGEDRESULTS, resCtrls, nullptr);
  if (!c) {
    return cookie;
  }
  ber_int_t estimate = 0;
  berval next{};
  if (ldap_parse_pageresponse_control(ld, c, &estimate, &next) ==
          LDAP_SUCCESS &&
      next.bv_val) {
    cookie.assign(next.bv_val, next.bv_len);
    ber_memfree(next.bv_val);
  }
  return cookie;
}

} // namespace

std::string Entry::first(const std::string &attribute) const {
  auto it = attributes.find(attribute);
  if (it == attributes.end() || it->second.empty()) {
    return {};
  }
  return it->second.front();
}

Connection::Connection(const Config &config) {
  // ldap_initialize only parses the URI; the TCP connection is opened lazily
  // by the first operation.
  int rc = ldap_initialize(&ld_, config.uri.c_str());
  if (rc != LDAP_SUCCESS) {
    throw LdapError("ldap_initialize(" + config.uri +
                        "): " + ldap_err2string(rc),
                    rc);
  }

  int version = LDAP_VERSION3;
  ldap_set_option(ld_, LDAP_OPT_PROTOCOL_VERSION, &version);
  // AD hands out referrals to other domains; chasing them would re-bind
  // anonymously, so ignore them.
  ldap_set_option(ld_, LDAP_OPT_REFERRALS, LDAP_OPT_OFF);

  timeval tv{.tv_sec = config.timeoutSeconds, .tv_usec = 0};
  ldap_set_option(ld_, LDAP_OPT_NETWORK_TIMEOUT, &tv);
  ldap_set_option(ld_, LDAP_OPT_TIMEOUT, &tv);

  // TLS settings must be set before LDAP_OPT_X_TLS_NEWCTX, which builds the
  // handle's TLS context. They apply to both ldaps:// and StartTLS.
  int reqCert = config.tlsVerify ? LDAP_OPT_X_TLS_DEMAND : LDAP_OPT_X_TLS_NEVER;
  ldap_set_option(ld_, LDAP_OPT_X_TLS_REQUIRE_CERT, &reqCert);
  if (!config.tlsCaFile.empty()) {
    ldap_set_option(ld_, LDAP_OPT_X_TLS_CACERTFILE, config.tlsCaFile.c_str());
  }
  int isServer = 0;
  ldap_set_option(ld_, LDAP_OPT_X_TLS_NEWCTX, &isServer);

  if (config.startTls) {
    rc = ldap_start_tls_s(ld_, nullptr, nullptr);
    if (rc != LDAP_SUCCESS) {
      fail(rc, "StartTLS");
    }
  }
}

Connection::~Connection() {
  if (ld_) {
    ldap_unbind_ext_s(ld_, nullptr, nullptr);
  }
}

void Connection::fail(int rc, const std::string &context) {
  std::string what = context + ": " + ldap_err2string(rc);
  // The server's diagnostic often says more, e.g. AD's
  // "80090308: LdapErr: ... data 52e" (52e = bad password, 775 = locked).
  char *diag = nullptr;
  if (ldap_get_option(ld_, LDAP_OPT_DIAGNOSTIC_MESSAGE,
                      static_cast<void *>(&diag)) == LDAP_OPT_SUCCESS &&
      diag) {
    if (*diag) {
      what += std::string(" (") + diag + ")";
    }
    ldap_memfree(diag);
  }
  throw LdapError(what, rc);
}

void Connection::bind(const std::string &dn, const std::string &password) {
  if (!dn.empty() && password.empty()) {
    throw LdapError("bind(" + dn + "): empty password not allowed",
                    LDAP_INVALID_CREDENTIALS);
  }
  berval cred{};
  cred.bv_val = const_cast<char *>(password.c_str());
  cred.bv_len = password.size();
  const int rc =
      ldap_sasl_bind_s(ld_, dn.empty() ? nullptr : dn.c_str(),
                       LDAP_SASL_SIMPLE, &cred, nullptr, nullptr, nullptr);
  if (rc != LDAP_SUCCESS) {
    fail(rc, "bind(" + dn + ")");
  }
}

std::vector<Entry>
Connection::search(const std::string &baseDn, Scope scope,
                   const std::string &filter,
                   const std::vector<std::string> &attributes) {
  std::vector<char *> attrs;
  attrs.reserve(attributes.size() + 1);
  for (const auto &a : attributes) {
    attrs.push_back(const_cast<char *>(a.c_str()));
  }
  attrs.push_back(nullptr);

  const std::string context = "search(" + baseDn + ", " + filter + ")";
  std::vector<Entry> entries;
  std::string cookie; // opaque paging cookie; empty = first/last page

  for (;;) {
    berval cookieVal{};
    cookieVal.bv_val = cookie.data();
    cookieVal.bv_len = cookie.size();
    LDAPControl *pageCtrl = nullptr;
    // Non-critical: servers without paging support just return everything.
    int rc = ldap_create_page_control(ld_, kPageSize, &cookieVal,
                                      /*iscritical=*/0, &pageCtrl);
    if (rc != LDAP_SUCCESS) {
      fail(rc, context);
    }
    std::array<LDAPControl *, 2> serverCtrls = {pageCtrl, nullptr};

    LDAPMessage *raw = nullptr;
    rc = ldap_search_ext_s(ld_, baseDn.c_str(), toLdapScope(scope),
                           filter.c_str(),
                           attributes.empty() ? nullptr : attrs.data(), 0,
                           serverCtrls.data(), nullptr, nullptr, LDAP_NO_LIMIT,
                           &raw);
    ldap_control_free(pageCtrl);
    const MessagePtr res(raw, &ldap_msgfree); // freed even on error
    if (rc != LDAP_SUCCESS) {
      fail(rc, context);
    }

    for (LDAPMessage *e = ldap_first_entry(ld_, res.get()); e;
         e = ldap_next_entry(ld_, e)) {
      entries.push_back(readEntry(ld_, e));
    }

    LDAPControl **resCtrls = nullptr;
    int err = LDAP_SUCCESS;
    rc = ldap_parse_result(ld_, res.get(), &err, nullptr, nullptr, nullptr,
                           &resCtrls, /*freeit=*/0);
    if (rc != LDAP_SUCCESS) {
      fail(rc, context);
    }
    cookie.clear();
    if (resCtrls) {
      cookie = nextPageCookie(ld_, resCtrls);
      ldap_controls_free(resCtrls);
    }
    if (cookie.empty()) {
      break;
    }
  }

  return entries;
}

std::string escapeFilterValue(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  for (const unsigned char c : value) {
    switch (c) {
    case '*':
    case '(':
    case ')':
    case '\\':
    case '\0':
      std::format_to(std::back_inserter(out), "\\{:02x}", c);
      break;
    default:
      out += static_cast<char>(c);
    }
  }
  return out;
}

namespace {

// LDAPv3 string form without optional spaces; empty if unparsable.
std::string normalizeDn(const std::string &dn) {
  char *out = nullptr;
  if (ldap_dn_normalize(dn.c_str(), LDAP_DN_FORMAT_LDAPV3, &out,
                        LDAP_DN_FORMAT_LDAPV3) != LDAP_SUCCESS) {
    return {};
  }
  std::string result = out ? out : "";
  ldap_memfree(out);
  return result;
}

bool equalsIgnoreCase(const std::string &a, const std::string &b) {
  const CaseInsensitiveLess less;
  return !less(a, b) && !less(b, a);
}

} // namespace

bool sameDn(const std::string &a, const std::string &b) {
  const std::string na = normalizeDn(a);
  const std::string nb = normalizeDn(b);
  if (na.empty() || nb.empty()) {
    return equalsIgnoreCase(a, b);
  }
  // Attribute types and the usual naming attributes (cn, ou, dc, uid) are
  // case-insensitive, which normalization alone doesn't account for.
  return equalsIgnoreCase(na, nb);
}

} // namespace detail
} // namespace ldapauth
