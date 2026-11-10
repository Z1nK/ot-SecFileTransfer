#pragma once
#include "transaction/transaction.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace confide::transaction {

// Version written as "v" in both documents; decode rejects other values.
inline constexpr int kDocumentVersion = 1;

// meta.json: every field of Transaction, including the file list, so the
// repository can rebuild its index from meta.json alone on startup.
std::string encode_meta(const Transaction& tx);

// `validation` on malformed JSON, missing fields, wrong version, bad id,
// path, digest, timestamp or state. meta.json is written by this server,
// but a damaged file must fail loudly rather than load half a transaction.
Result<Transaction> decode_meta(std::string_view json);

// manifest.json: written once at commit, the file list with sizes and
// sha256. Also the file list sent to a peer when forwarding.
std::string encode_manifest(const Transaction& tx);

// Same errors as decode_meta(). 
// Only "v" and "files" are read; the caller already knows the id.
Result<std::vector<FileEntry>> decode_manifest(std::string_view json);

// Both decoders return the files sorted by path and reject duplicate paths,
// so Transaction::files stays sorted whatever order the input had.

}  // namespace confide::transaction
