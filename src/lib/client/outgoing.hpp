#pragma once
// Building an outgoing transaction: create, upload, commit. In ftc, shared by
// `send` (all steps at once) and `create` / `add` / `rm` / `commit` (one step each).
#include "client.hpp"
#include "local_files.hpp"

#include <common/id.hpp>
#include <net/http_client.hpp>

#include <boost/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace confide::client {

struct NewTransaction {
  std::string to;  // USER or GROUP, optionally @SERVER
  std::optional<int> retention_days;
  std::optional<std::string> comment;
};

// POST /transactions; returns the new transaction's id.
Result<common::TransactionId> create_transaction(net::HttpClient& client,
                                                 const NewTransaction& req);

// Called after each file is stored, e.g. to show progress.
using OnUploaded = std::function<void(const LocalFile&)>;

// Uploads each file with its checksum, calling `on_uploaded` once it is stored.
// A file already in the transaction under the same path is replaced.
Result<void> upload_files(net::HttpClient& client, const common::TransactionId& id,
                          const std::vector<LocalFile>& files,
                          const OnUploaded& on_uploaded = {});

// Commits; returns the transaction as the server sent it back.
Result<boost::json::object> commit_transaction(net::HttpClient& client,
                                               const common::TransactionId& id);

}  // namespace confide::client
