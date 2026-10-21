#include "reply.hpp"

#include <format>
#include <utility>

namespace confide::cli {

Result<json::object> parse_object(std::string_view text) {
  boost::system::error_code ec;
  json::value v = json::parse(text, ec);
  if (ec || !v.is_object()) {
    return fail(ErrCode::internal, "server sent malformed JSON");
  }
  return std::move(v.get_object());
}

Result<json::array> parse_list(std::string_view text, std::string_view key) {
  CFD_TRY(obj, parse_object(text));
  auto* arr = obj.if_contains(key);
  if (arr == nullptr || !arr->is_array()) {
    return fail(ErrCode::internal, std::format("server reply has no '{}' list", key));
  }
  return std::move(arr->get_array());
}

std::string_view str(const json::object& obj, std::string_view key) {
  const auto* v = obj.if_contains(key);
  return v != nullptr && v->is_string() ? std::string_view{v->get_string()} : std::string_view{};
}

std::uint64_t num(const json::object& obj, std::string_view key) {
  const auto* v = obj.if_contains(key);
  if (v == nullptr) {
    return 0;
  }
  if (v->is_uint64()) {
    return v->get_uint64();
  }
  return v->is_int64() && v->get_int64() > 0 ? static_cast<std::uint64_t>(v->get_int64()) : 0;
}

std::string address(const json::object& tx, std::string_view user_key,
                    std::string_view server_key) {
  std::string out{str(tx, user_key)};
  if (const auto server = str(tx, server_key); !server.empty()) {
    out += '@';
    out += server;
  }
  return out;
}

}  // namespace confide::cli
