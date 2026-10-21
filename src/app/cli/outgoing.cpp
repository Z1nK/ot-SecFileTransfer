#include "outgoing.hpp"

#include "reply.hpp"

#include <boost/beast/http/verb.hpp>

#include <iostream>

namespace confide::cli {

namespace {

using http_verb = boost::beast::http::verb;

}  // namespace

Result<common::TransactionId> create_transaction(net::HttpClient& client,
                                                 const NewTransaction& req) {
  const auto at = req.to.rfind('@');
  json::object body{{"target_user", req.to.substr(0, at)}};
  if (at != std::string::npos) {
    body["target_server"] = req.to.substr(at + 1);
  }
  if (req.retention_days) {
    body["retention_days"] = *req.retention_days;
  }
  if (req.comment) {
    body["comment"] = *req.comment;
  }

  CFD_TRY(created, client.call(http_verb::post, "/transactions", json::serialize(body)));
  CFD_TRY(tx, parse_object(created));
  return common::TransactionId::parse(str(tx, "id"));
}

Result<void> upload_files(net::HttpClient& client, const common::TransactionId& id,
                          const std::vector<LocalFile>& files) {
  for (const auto& file : files) {
    CFD_TRY(sha, hash_file(file.source));
    CFD_TRYV(client.upload(tx_target(id) + "/files/" + net::url_encode_path(file.path.str()),
                           file.source, sha));
    std::cout << "  " << file.path.str() << '\n';
  }
  return {};
}

Result<json::object> commit_transaction(net::HttpClient& client,
                                        const common::TransactionId& id) {
  CFD_TRY(reply, client.call(http_verb::post, tx_target(id) + "/commit"));
  return parse_object(reply);
}

}  // namespace confide::cli
