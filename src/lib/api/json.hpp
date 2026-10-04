#pragma once
#include "transaction/service.hpp"
#include "transaction/transaction.hpp"

#include <string>
#include <string_view>
#include <vector>

// JSON bodies of the REST API. Boost.JSON stays inside
// json.cpp; no public header includes it.

namespace confide::api {

using common::Result;

// --- Requests ---------------------------------------------------------------

// POST /transactions
//   {"target_user":"bob","target_server":"siteB","retention_days":30}
// target_server and retention_days are optional. `validation` on malformed
// JSON, a missing or empty target_user, or a non-integer retention_days.
Result<transaction::CreateRequest> decode_create(std::string_view json);

// POST /internal/transactions (peer push, Architecture §7)
//   {"id":"...","sender":"alice","target_user":"bob",
//    "created_at":"...","expires_at":"..."}
// Same errors as decode_create(), plus a bad id or timestamp.
Result<transaction::ReceiveRequest> decode_receive(std::string_view json);

// The body decode_receive() reads; used by the forwarder on the other side.
std::string encode_receive(const transaction::ReceiveRequest& req);

// --- Responses --------------------------------------------------------------

// GET /transactions/{id}, and the answer to create, receive and commit:
//   {"id":"...","state":"open","sender":"alice","target_user":"bob",
//    "target_server":"siteB","created_at":"...","expires_at":"...",
//    "committed_at":"...","file_count":2,"total_bytes":12345}
// Optional times are left out while unset.
std::string encode_transaction(const transaction::Transaction& tx);

// GET /transactions?box=in|out
//   {"transactions":[<encode_transaction() without files>, ...]}
std::string encode_transaction_list(const std::vector<transaction::Transaction>& txs);

// GET /transactions/{id}/files
//   {"files":[{"path":"report/a.csv","size":10240,"sha256":"..."}, ...]}
std::string encode_files(const std::vector<transaction::FileEntry>& files);

// PUT .../files/{path}: the stored entry.
//   {"path":"report/a.csv","size":10240,"sha256":"..."}
std::string encode_file(const transaction::FileEntry& file);

}  // namespace confide::api
