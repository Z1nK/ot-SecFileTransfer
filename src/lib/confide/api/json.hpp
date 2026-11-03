#pragma once
#include "api/authenticator.hpp"
#include "transaction/service.hpp"
#include "transaction/transaction.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// JSON bodies of the REST API. Boost.JSON stays inside
// json.cpp; no public header includes it.

namespace confide::api {

using common::Result;

// --- Requests ---------------------------------------------------------------

// POST /transactions
//   {"target_user":"bob","target_server":"siteB","retention_days":30,
//    "comment":"Q3 reports"}
// target_server, retention_days and comment are optional; the comment's
// length is checked by TransactionService. `validation` on malformed
// JSON, a missing or empty target_user, or a non-integer retention_days.
Result<transaction::CreateRequest> decode_create(std::string_view json);

// POST /internal/transactions (peer push, Architecture §7)
//   {"id":"...","sender":"alice","target_user":"bob",
//    "created_at":"...","expires_at":"...","comment":"..."}
// comment is optional. Same errors as decode_create(), plus a bad id or timestamp.
Result<transaction::ReceiveRequest> decode_receive(std::string_view json);

// PUT /admin/transactions/{id}/retention
//   {"days":30}
// The range is checked by TransactionService.
Result<int> decode_retention(std::string_view json);

// The body decode_receive() reads; used by the forwarder on the other side.
std::string encode_receive(const transaction::ReceiveRequest& req);

// --- Responses --------------------------------------------------------------

// GET /transactions/{id}, and the answer to create, receive and commit:
//   {"id":"...","state":"open","sender":"alice","target_user":"bob",
//    "target_server":"siteB","comment":"...","created_at":"...","expires_at":"...",
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

// POST /auth/token
//   {"token":"ftu_...","user":"alice","expires_at":"..."}
std::string encode_token(std::string_view token, std::string_view user,
                         common::Timestamp expires_at);

// --- Admin responses --------------------------------------------------------

// GET /admin/transactions/{id}: encode_transaction() plus "origin_server"
// and the file list (metadata only):
//   {..., "origin_server":"siteB", "files":[{"path","size","sha256"}, ...]}
std::string encode_admin_transaction(const transaction::Transaction& tx);

// GET /admin/transactions
//   {"transactions":[<encode_transaction() plus "origin_server">, ...]}
std::string encode_admin_transaction_list(const std::vector<transaction::Transaction>& txs);

// GET /admin/sessions
//   {"sessions":[{"user":"bob","scope":"user","created":"...","last_used":"...",
//                 "expires_at":"..."}, ...]}
std::string encode_sessions(const std::vector<SessionView>& sessions);

// DELETE /admin/sessions/{user}
//   {"user":"bob","revoked":2}
std::string encode_revoked(std::string_view user, std::size_t count);

struct PeerView {
  std::string name;
  std::string host;
  std::uint16_t port = 0;
};

// GET /admin/status
struct ServerStatus {
  std::string instance;
  std::string version;
  std::string auth_provider;  // "local" or "ldap"
  common::Timestamp started_at;
  common::Timestamp now;
  std::vector<PeerView> peers;
  std::map<std::string, std::size_t> transactions;  // state name -> count
  std::size_t outgoing = 0;                         // committed, waiting to be forwarded
  std::size_t sessions = 0;
};

//   {"instance":"siteA","version":"1.2.0","auth_provider":"local",
//    "started_at":"...","uptime_s":3600,"peers":[{"name","host","port"}],
//    "transactions":{"open":1,"committed":4},"outgoing":0,"sessions":3}
std::string encode_status(const ServerStatus& status);

}  // namespace confide::api
