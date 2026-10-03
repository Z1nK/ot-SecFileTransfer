#include "transaction/codec.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace confide::transaction {

namespace {

namespace json = boost::json;

using common::ErrCode;
using common::Error;

std::unexpected<Error> invalid(std::string detail) {
  return std::unexpected(Error::make(ErrCode::validation, std::move(detail)));
}

// Prefixes a parse error from common (id, path, digest, time) with the field
// name, so "bad hex digit" becomes "sha256: bad hex digit".
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
    return invalid("JSON document is not an object");
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

// Boost.JSON stores non-negative numbers that fit as int64, larger ones as
// uint64; accept both, reject negatives and floats.
Result<std::uint64_t> get_uint64(const json::object& obj, std::string_view key) {
  const auto* v = obj.if_contains(key);
  if (v != nullptr) {
    if (v->is_uint64()) {
      return v->get_uint64();
    }
    if (v->is_int64() && v->get_int64() >= 0) {
      return static_cast<std::uint64_t>(v->get_int64());
    }
  }
  return invalid("missing or non-negative integer field: " + std::string{key});
}

Result<common::Timestamp> get_timestamp(const json::object& obj, std::string_view key) {
  CFD_TRY(text, get_string(obj, key));
  return in_field(key, common::parse_iso8601(text));
}

// Absent or null -> nullopt.
Result<std::optional<common::Timestamp>> get_optional_timestamp(const json::object& obj,
                                                                std::string_view key) {
  const auto* v = obj.if_contains(key);
  if (v == nullptr || v->is_null()) {
    return std::nullopt;
  }
  CFD_TRY(ts, get_timestamp(obj, key));
  return ts;
}

Result<void> check_version(const json::object& obj) {
  CFD_TRY(v, get_uint64(obj, "v"));
  if (v != kDocumentVersion) {
    return invalid("unsupported document version: " + std::to_string(v));
  }
  return {};
}

json::array encode_files(const std::vector<FileEntry>& files) {
  json::array arr;
  arr.reserve(files.size());
  for (const auto& f : files) {
    arr.push_back(json::object{
        {"path", f.path.str()},
        {"size", f.size},
        {"sha256", f.sha256.to_hex()},
    });
  }
  return arr;
}

Result<std::vector<FileEntry>> decode_files(const json::object& obj) {
  const auto* v = obj.if_contains("files");
  if (v == nullptr || !v->is_array()) {
    return invalid("missing or non-array field: files");
  }

  std::vector<FileEntry> files;
  files.reserve(v->get_array().size());
  for (const auto& item : v->get_array()) {
    if (!item.is_object()) {
      return invalid("files: entry is not an object");
    }
    const auto& f = item.get_object();
    CFD_TRY(path_text, get_string(f, "path"));
    CFD_TRY(path, in_field("path", common::RelativePath::parse(path_text)));
    CFD_TRY(size, get_uint64(f, "size"));
    CFD_TRY(hex, get_string(f, "sha256"));
    CFD_TRY(sha256, in_field("sha256", common::Sha256Digest::from_hex(hex)));
    files.push_back({.path = std::move(path), .size = size, .sha256 = sha256});
  }

  std::ranges::sort(files, {}, &FileEntry::path);
  const auto dup = std::ranges::adjacent_find(files, {}, &FileEntry::path);
  if (dup != files.end()) {
    return invalid("files: duplicate path " + dup->path.str());
  }
  return files;
}

}  // namespace

std::string encode_meta(const Transaction& tx) {
  json::object obj{
      {"v", kDocumentVersion},
      {"id", tx.id.str()},
      {"sender", tx.sender},
      {"target_user", tx.target_user},
      {"target_server", tx.target_server},
      {"origin_server", tx.origin_server},
      {"state", to_string(tx.state)},
      {"created_at", common::format_iso8601(tx.created_at)},
      {"expires_at", common::format_iso8601(tx.expires_at)},
  };
  if (tx.committed_at) {
    obj["committed_at"] = common::format_iso8601(*tx.committed_at);
  }
  if (tx.delivered_at) {
    obj["delivered_at"] = common::format_iso8601(*tx.delivered_at);
  }
  obj["files"] = encode_files(tx.files);
  return json::serialize(obj);
}

Result<Transaction> decode_meta(std::string_view text) {
  CFD_TRY(obj, parse_object(text));
  CFD_TRYV(check_version(obj));

  CFD_TRY(id_text, get_string(obj, "id"));
  CFD_TRY(id, in_field("id", common::TransactionId::parse(id_text)));
  CFD_TRY(sender, get_name(obj, "sender"));
  CFD_TRY(target_user, get_name(obj, "target_user"));
  CFD_TRY(target_server, get_string(obj, "target_server"));
  CFD_TRY(origin_server, get_string(obj, "origin_server"));
  CFD_TRY(state_text, get_string(obj, "state"));
  CFD_TRY(state, in_field("state", parse_state(state_text)));
  CFD_TRY(created_at, get_timestamp(obj, "created_at"));
  CFD_TRY(expires_at, get_timestamp(obj, "expires_at"));
  CFD_TRY(committed_at, get_optional_timestamp(obj, "committed_at"));
  CFD_TRY(delivered_at, get_optional_timestamp(obj, "delivered_at"));
  CFD_TRY(files, decode_files(obj));

  return Transaction{
      .id = std::move(id),
      .sender = std::string{sender},
      .target_user = std::string{target_user},
      .target_server = std::string{target_server},
      .origin_server = std::string{origin_server},
      .state = state,
      .created_at = created_at,
      .expires_at = expires_at,
      .committed_at = committed_at,
      .delivered_at = delivered_at,
      .files = std::move(files),
  };
}

std::string encode_manifest(const Transaction& tx) {
  json::object obj{
      {"v", kDocumentVersion},
      {"id", tx.id.str()},
  };
  obj["files"] = encode_files(tx.files);
  return json::serialize(obj);
}

Result<std::vector<FileEntry>> decode_manifest(std::string_view text) {
  CFD_TRY(obj, parse_object(text));
  CFD_TRYV(check_version(obj));
  return decode_files(obj);
}

}  // namespace confide::transaction
