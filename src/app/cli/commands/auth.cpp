// ftc login, ftc logout.
#include "commands.hpp"
#include "session.hpp"

#include <iostream>

namespace confide::cli {

namespace {

Result<void> run_login(const Global& global, const po::variables_map& /*vm*/) {
  CFD_TRY(session, open_session(global));
  CFD_TRY(reply, login(session.client, global.user, session.cache));
  std::cout << "logged in as " << reply.user << " until " << reply.expires_at << '\n';
  return {};
}

Result<void> run_logout(const Global& global, const po::variables_map& /*vm*/) {
  CFD_TRY(session, open_session(global));
  CFD_TRY(saved, session.cache.load());
  if (!saved) {
    std::cout << "not logged in\n";
    return {};
  }
  session.client.set_token(*saved);
  auto revoked = session.client.logout();
  // The local copy goes in any case; an expired token is gone on the server
  // already.
  CFD_TRYV(session.cache.remove());
  if (!revoked && revoked.error().code != ErrCode::auth) {
    std::cerr << "ftc: warning: server not told (" << describe(revoked.error())
              << "); the token stays valid there until it expires\n";
  }
  std::cout << "logged out\n";
  return {};
}

}  // namespace

const Command kLogin{.name = "login", .synopsis = "", .run = run_login};
const Command kLogout{.name = "logout", .synopsis = "", .run = run_logout};

}  // namespace confide::cli
