#pragma once
// Runs blocking calls (file I/O, fsync, LDAP) off the network threads.
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <optional>
#include <type_traits>
#include <utility>

namespace confide::api {

namespace asio = boost::asio;

// Calls `f()` on `pool` and resumes the caller on its own executor (the
// connection's strand) with the result; an exception from `f` is rethrown
// here. The caller stays suspended meanwhile, so `f` may capture its locals
// by reference. Must not be called with a lock held across the co_await.
template <class F>
asio::awaitable<std::invoke_result_t<F&>> offload(const asio::any_io_executor& pool, F f) {
  using R = std::invoke_result_t<F&>;
  static_assert(!std::is_void_v<R>, "offload() needs a result; return Result<void>");
  // optional: R needn't be default-constructible.
  std::optional<R> result;
  co_await asio::co_spawn(
      pool,
      [&]() -> asio::awaitable<void> {
        result.emplace(f());
        co_return;
      },
      asio::use_awaitable);
  co_return std::move(*result);
}

}  // namespace confide::api
