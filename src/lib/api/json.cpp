#include "api/json.hpp"

#include <boost/json.hpp>

#include <limits>
#include <string>
#include <utility>

namespace confide::api {

namespace {

namespace json = boost::json;

using common::ErrCode;
using common::Error;

std::unexpected<Error> invalid(std::string detail) {
  return std::unexpected(Error::make(ErrCode::validation, std::move(detail)));
}

// Prefixes a parse error from common (id, time) with the field name.
template <class T>
Result<T> in_field(std::string_view key, Result<T> r) {
  if (!r) {
    return invalid(std::string{key} + ": " + r.error().detail);
  }
  return r;
}

Result<json::object> parse_object(std::string_view text) {
  boost::system::error_code ec;
  json::value v = json::parse(text, ec);
  if (ec) {
    return invalid("malformed JSON: " + ec.message());
  }
  if (!v.is_object()) {
    return invalid("JSON body is not an object");
  }
  return std::move(v.get_object());
}

// The returned view points into `obj`.
Result<std::string_view> get_string(const json::object& obj, std::string_view key) {
  const auto* v = obj.if_contains(key);
  if (v == nullptr || !v->is_string()) {
    return invalid("missing or non-string field: " + std::string{key});
  }
  return std::string_view{v->get_string()};
}

// Like get_string() but also rejects "".
Result<std::string_view> get_name(const json::object& obj, std::string_view key) {
  CFD_TRY(s, get_string(obj, key));
  if (s.empty()) {
    return invalid("empty field: " + std::string{key});
  }
  return s;
}

// Absent or null -> "".
Result<std::string_view> get_optional_string(const json::object& obj, std::string_view key) {
  const auto* v = obj.if_contains(key);
  if (v == nullptr || v->is_null()) {
    return std::string_view{};
  }
  return get_string(obj, key);
}

// Absent or null -> 0. The range is checked by TransactionService.
Result<int> get_optional_int(const json::object& obj, std::string_view key) {
  const auto* v = obj.if_contains(key);
  if (v == nullptr || v->is_null()) {
    return 0;
  }
  if (v->is_int64()) {
    const auto n = v->get_int64();
    if (n >= std::numeric_limits<int>::min() && n <= std::numeric_limits<int>::max()) {
      return static_cast<int>(n);
    }
  }
  return invalid("field is not a small integer: " + std::string{key});
}

Result<common::Timestamp> get_timestamp(const json::object& obj, std::string_view key) {
  CFD_TRY(text, get_string(obj, key));
  return in_field(key, common::parse_iso8601(text));
}

json::object file_object(const transaction::FileEntry& f) {
  return json::object{
      {"path", f.path.str()},
      {"size", f.size},
      {"sha256", f.sha256.to_hex()},
  };
}

json::object transaction_object(const transaction::Transaction& tx) {
  json::object obj{
      {"id", tx.id.str()},
      {"state", transaction::to_string(tx.state)},
      {"sender", tx.sender},
      {"target_user", tx.target_user},
      {"target_server", tx.target_server},
      {"created_at", common::format_iso8601(tx.created_at)},
      {"expires_at", common::format_iso8601(tx.expires_at)},
  };
  if (tx.committed_at) {
    obj["committed_at"] = common::format_iso8601(*tx.committed_at);
  }
  if (tx.delivered_at) {
    obj["delivered_at"] = common::format_iso8601(*tx.delivered_at);
  }
  obj["file_count"] = tx.files.size();
  obj["total_bytes"] = tx.total_bytes();
  return obj;
}

}  // namespace

Result<transaction::CreateRequest> decode_create(std::string_view text) {
  CFD_TRY(obj, parse_object(text));
  CFD_TRY(target_user, get_name(obj, "target_user"));
  CFD_TRY(target_server, get_optional_string(obj, "target_server"));
  CFD_TRY(retention_days, get_optional_int(obj, "retention_days"));
  return transaction::CreateRequest{
      .target_user = std::string{target_user},
      .target_server = std::string{target_server},
      .retention_days = retention_days,
  };
}

Result<transaction::ReceiveRequest> decode_receive(std::string_view text) {
  CFD_TRY(obj, parse_object(text));
  CFD_TRY(id_text, get_string(obj, "id"));
  CFD_TRY(id, in_field("id", common::TransactionId::parse(id_text)));
  CFD_TRY(sender, get_name(obj, "sender"));
  CFD_TRY(target_user, get_name(obj, "target_user"));
  CFD_TRY(created_at, get_timestamp(obj, "created_at"));
  CFD_TRY(expires_at, get_timestamp(obj, "expires_at"));
  return transaction::ReceiveRequest{
      .id = std::move(id),
      .sender = std::string{sender},
      .target_user = std::string{target_user},
      .created_at = created_at,
      .expires_at = expires_at,
  };
}

std::string encode_receive(const transaction::ReceiveRequest& req) {
  return json::serialize(json::object{
      {"id", req.id.str()},
      {"sender", req.sender},
      {"target_user", req.target_user},
      {"created_at", common::format_iso8601(req.created_at)},
      {"expires_at", common::format_iso8601(req.expires_at)},
  });
}

std::string encode_transaction(const transaction::Transaction& tx) {
  return json::serialize(transaction_object(tx));
}

std::string encode_transaction_list(const std::vector<transaction::Transaction>& txs) {
  json::array arr;
  arr.reserve(txs.size());
  for (const auto& tx : txs) {
    arr.push_back(transaction_object(tx));
  }
  return json::serialize(json::object{{"transactions", std::move(arr)}});
}

std::string encode_files(const std::vector<transaction::FileEntry>& files) {
  json::array arr;
  arr.reserve(files.size());
  for (const auto& f : files) {
    arr.push_back(file_object(f));
  }
  return json::serialize(json::object{{"files", std::move(arr)}});
}

std::string encode_file(const transaction::FileEntry& file) {
  return json::serialize(file_object(file));
}

}  // namespace confide::api
