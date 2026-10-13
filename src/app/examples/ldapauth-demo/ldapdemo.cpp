// ldapauth-demo: read-only CLI showing how an application uses ldapauth.
//
// Settings come from environment variables (defaults match the test server
// from docker/docker-compose.yml):
//   LDAP_URI           ldap://localhost:1389
//   LDAP_BASE_DN       dc=example,dc=org
//   LDAP_USER_BASE     ou=people,<LDAP_BASE_DN>
//   LDAP_GROUP_BASE    ou=groups,<LDAP_BASE_DN>
//   LDAP_BIND_DN       cn=admin,dc=example,dc=org   (service account)
//   LDAP_BIND_PW       adminpassword
//   LDAP_AD            0 | 1   use Active Directory defaults
//   LDAP_STARTTLS      0 | 1
//   LDAP_TLS_INSECURE  0 | 1   skip server cert verification (test only)
//   LDAP_TLS_CACERT    path to CA file to trust

#include "ldapclient/ldapAuth.hpp"

#include <termios.h>
#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string env(const char *name, const std::string &fallback) {
  const char *v = std::getenv(name);
  return v && *v ? v : fallback;
}

ldapauth::Config configFromEnv() {
  auto c = env("LDAP_AD", "0") == "1" ? ldapauth::Config::activeDirectory()
                                      : ldapauth::Config{};
  std::string base = env("LDAP_BASE_DN", "dc=example,dc=org");
  c.uri = env("LDAP_URI", "ldap://localhost:1389");
  c.startTls = env("LDAP_STARTTLS", "0") == "1";
  c.tlsVerify = env("LDAP_TLS_INSECURE", "0") != "1";
  c.tlsCaFile = env("LDAP_TLS_CACERT", "");
  c.bindDn = env("LDAP_BIND_DN", "cn=admin,dc=example,dc=org");
  c.bindPassword = env("LDAP_BIND_PW", "adminpassword");
  c.userBaseDn = env("LDAP_USER_BASE", "ou=people," + base);
  c.groupBaseDn = env("LDAP_GROUP_BASE", "ou=groups," + base);
  return c;
}

// Read a password from the terminal without echo.
std::string promptPassword() {
  std::cerr << "Password: ";
  termios old{};
  bool tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
  if (tty) {
    termios noEcho = old;
    noEcho.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &noEcho);
  }
  std::string pw;
  std::getline(std::cin, pw);
  if (tty) {
    tcsetattr(STDIN_FILENO, TCSANOW, &old);
    std::cerr << '\n';
  }
  return pw;
}

void printUser(const ldapauth::User &u) {
  std::cout << "  dn:       " << u.dn << "\n  username: " << u.username
            << "\n  name:     " << u.displayName << "\n  mail:     " << u.mail
            << "\n  groups:  ";
  for (const auto &g : u.groups)
    std::cout << ' ' << g;
  std::cout << '\n';
  for (const auto &dn : u.groupDns)
    std::cout << "  group dn: " << dn << '\n';
}

int usage() {
  std::cerr << "usage: ldapauth-demo <command> [args]\n"
               "  auth <user> [password]     log in (prompts if no password)\n"
               "  user <user>                show a user and their groups\n"
               "  is-member <user> <group>   exit 0 if user is in group\n"
               "                             (group = name, or DN if it has '=')\n"
               "  groups                     list all groups\n"
               "  group <name>               show a group and its members\n";
  return 2;
}

} // namespace

int main(int argc, char **argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty())
    return usage();

  ldapauth::LdapAuth ldap(configFromEnv());
  const std::string &cmd = args[0];

  try {
    if (cmd == "auth" && (args.size() == 2 || args.size() == 3)) {
      std::string pw = args.size() == 3 ? args[2] : promptPassword();
      auto r = ldap.authenticate(args[1], pw);
      if (!r) {
        // An app would show only "invalid username or password" to the user
        // and log the details.
        std::cout << "DENIED: " << toString(r.status) << " (" << r.message
                  << ")\n";
        return 1;
      }
      std::cout << "OK\n";
      printUser(*r.user);
    } else if (cmd == "user" && args.size() == 2) {
      auto u = ldap.findUser(args[1]);
      if (!u) {
        std::cerr << "no such user: " << args[1] << '\n';
        return 1;
      }
      printUser(*u);
    } else if (cmd == "is-member" && args.size() == 3) {
      auto u = ldap.findUser(args[1]);
      // Checking by DN is exact; by name, any group with that name counts.
      bool byDn = args[2].find('=') != std::string::npos;
      bool member = u && (byDn ? u->inGroupDn(args[2]) : u->inGroup(args[2]));
      std::cout << (member ? "yes" : "no") << '\n';
      return member ? 0 : 1;
    } else if (cmd == "groups" && args.size() == 1) {
      for (const auto &g : ldap.listGroups())
        std::cout << g.name << '\t' << g.description << '\n';
    } else if (cmd == "group" && args.size() == 2) {
      auto g = ldap.findGroup(args[1]);
      if (!g) {
        std::cerr << "no such group: " << args[1] << '\n';
        return 1;
      }
      std::cout << g->name << "  (" << g->dn << ")\n";
      if (!g->description.empty())
        std::cout << "  description: " << g->description << '\n';
      for (const auto &m : g->members)
        std::cout << "  member: " << m << '\n';
    } else {
      return usage();
    }
  } catch (const ldapauth::LdapError &e) {
    // Directory unusable (down, TLS, service account, config): not the
    // user's fault, so don't report it as "wrong password".
    std::cerr << "LDAP error " << e.code() << ": " << e.what() << '\n';
    return 3;
  }
  return 0;
}
