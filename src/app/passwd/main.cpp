// Prints a `password_hash` for a [[user]] in the server config
// (auth::PasswordHash, PBKDF2-HMAC-SHA256).
//
//   read -rs PW && printf '%s\n' "$PW" | confide-passwd
//   confide-passwd --iterations 1000000 < password.txt
//
// The password is the first line of stdin, without the line ending, so it
// never appears in the shell history or in `ps`.

#include <auth/password.hpp>

#include <charconv>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

using confide::auth::PasswordHash;

namespace {

void usage(std::string_view prog) {
  std::cerr << "usage: " << prog << " [--iterations N] < password\n"
            << "  reads the password from the first line of stdin and prints its hash\n"
            << "  default iterations: " << PasswordHash::kDefaultIterations << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  const std::span args(argv, static_cast<std::size_t>(argc));
  std::uint32_t iterations = PasswordHash::kDefaultIterations;

  if (args.size() == 3 && std::string_view(args[1]) == "--iterations") {
    const std::string_view text(args[2]);
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), iterations);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
      usage(args[0]);
      return 2;
    }
  } else if (args.size() != 1) {
    usage(args[0]);
    return 2;
  }

  std::string password;
  if (!std::getline(std::cin, password)) {
    std::cerr << "no password on stdin\n";
    return 1;
  }
  if (password.ends_with('\r')) {
    password.pop_back();
  }
  if (password.empty()) {
    std::cerr << "empty password\n";
    return 1;
  }

  auto hash = PasswordHash::create(password, iterations);
  if (!hash) {
    std::cerr << hash.error().detail << '\n';
    return 1;
  }
  std::cout << hash->to_string() << '\n';
  return 0;
}
