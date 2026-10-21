// ftc get: download and verify all files of a transaction.
#include "commands.hpp"
#include "local_files.hpp"
#include "reply.hpp"
#include "session.hpp"

#include <boost/beast/http/verb.hpp>

#include <filesystem>
#include <format>
#include <iostream>
#include <system_error>

namespace confide::cli {

namespace {

namespace fs = std::filesystem;
using http_verb = boost::beast::http::verb;

void get_options(po::options_description& desc, po::options_description& hidden,
                 po::positional_options_description& pos) {
  desc.add_options()("force,f", "overwrite existing files");
  hidden.add_options()("id", po::value<std::string>()->required())(
      "dir", po::value<std::string>()->default_value("."));
  pos.add("id", 1).add("dir", 1);
}

Result<void> run_get(const Global& global, const po::variables_map& vm) {
  CFD_TRY(id, common::TransactionId::parse(vm["id"].as<std::string>()));
  const fs::path dir = vm["dir"].as<std::string>();
  const bool force = vm.contains("force");

  CFD_TRY(client, connect(global));
  CFD_TRY(reply, client.call(http_verb::get, tx_target(id) + "/files"));
  CFD_TRY(list, parse_list(reply, "files"));

  for (const auto& v : list) {
    if (!v.is_object()) {
      return fail(ErrCode::internal, "server sent a malformed file list");
    }
    const auto& f = v.get_object();
    // Paths from the server are untrusted: no "../" may escape `dir`.
    CFD_TRY(rel, common::RelativePath::parse(str(f, "path")));
    CFD_TRY(expected, common::Sha256Digest::from_hex(str(f, "sha256")));

    const auto out = rel.under(dir);
    std::error_code ec;
    if (!force && fs::exists(out, ec)) {
      return fail(ErrCode::conflict, out.string() + " exists (use --force to overwrite)");
    }
    fs::create_directories(out.parent_path(), ec);
    if (ec) {
      return fail(ErrCode::io, std::format("{}: {}", out.parent_path().string(), ec.message()));
    }

    // Downloaded next to the target and renamed once the checksum matches.
    auto part = out;
    part += ".part";
    auto checked = [&]() -> Result<void> {
      CFD_TRYV(client.download(tx_target(id) + "/files/" + net::url_encode_path(rel.str()), part));
      CFD_TRY(actual, hash_file(part));
      if (actual != expected) {
        return fail(ErrCode::integrity, rel.str() + ": SHA-256 does not match the server's");
      }
      fs::rename(part, out, ec);
      if (ec) {
        return fail(ErrCode::io, std::format("{}: {}", out.string(), ec.message()));
      }
      return {};
    };
    if (auto res = checked(); !res) {
      fs::remove(part, ec);
      return res;
    }
    std::cout << "  " << out.string() << '\n';
  }
  std::cout << "downloaded " << list.size() << " files into " << dir.string() << '\n';
  return {};
}

}  // namespace

const Command kGet{
    .name = "get",
    .synopsis = " ID [DIR] [--force]",
    .options = get_options,
    .run = run_get,
};

}  // namespace confide::cli
