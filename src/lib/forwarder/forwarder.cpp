#include "forwarder/forwarder.hpp"

#include "api/json.hpp"

#include <boost/beast/http/verb.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>

namespace confide::forwarder {

namespace {

namespace json = boost::json;
using http_verb = boost::beast::http::verb;

using common::ErrCode;
using common::Error;

constexpr std::string_view kTag = "forwarder";
constexpr std::string_view kInternal = "/internal/transactions";

std::unexpected<Error> fail(ErrCode code, std::string detail) {
  return std::unexpected(Error::make(code, std::move(detail)));
}

std::string describe(const Error& err) {
  return std::format("[{}] {}", common::to_string(err.code), err.detail);
}

// "state" of a transaction as the peer's API encodes it.
Result<std::string> state_of(std::string_view body) {
  boost::system::error_code ec;
  const json::value v = json::parse(body, ec);
  const auto* obj = ec ? nullptr : v.if_object();
  const auto* state = obj == nullptr ? nullptr : obj->if_contains("state");
  if (state == nullptr || !state->is_string()) {
    return fail(ErrCode::internal, "peer sent a transaction without a state");
  }
  return std::string{state->get_string()};
}

}  // namespace

Forwarder::Forwarder(transaction::TransactionService& service, const storage::FileStore& store,
                     logging::TechLog& tlog, ForwarderOptions options)
    : service_(service), store_(store), tlog_(tlog), options_(std::move(options)) {}

Forwarder::~Forwarder() {
  stop();
}

bool Forwarder::is_transient(ErrCode code) {
  switch (code) {
  case ErrCode::unavailable:
  case ErrCode::throttled:
  case ErrCode::internal:
  case ErrCode::io:
  case ErrCode::integrity:
    return true;
  default:
    return false;
  }
}

void Forwarder::enqueue(const common::TransactionId& id) {
  {
    const std::scoped_lock lock(mu_);
    if (std::ranges::find(queue_, id, &Pending::id) != queue_.end()) {
      return;
    }
    queue_.push_back(Pending{.id = id, .attempts = 0, .due = std::chrono::steady_clock::now()});
  }
  cv_.notify_all();
}

void Forwarder::start() {
  if (worker_.joinable()) {
    return;
  }
  const auto waiting = service_.list_outgoing();
  for (const auto& id : waiting) {
    enqueue(id);
  }
  if (!waiting.empty()) {
    tlog_.info(kTag, "{} transaction(s) from before still to forward", waiting.size());
  }
  {
    const std::scoped_lock lock(mu_);
    stop_ = false;
  }
  worker_ = std::thread([this] { run(); });
}

void Forwarder::stop() {
  {
    const std::scoped_lock lock(mu_);
    stop_ = true;
    if (active_ != nullptr) {
      active_->cancel();
    }
  }
  cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

std::size_t Forwarder::queued() const {
  const std::scoped_lock lock(mu_);
  return queue_.size();
}

const Peer* Forwarder::find_peer(std::string_view name) const {
  const auto it = std::ranges::find(options_.peers, name, &Peer::name);
  return it == options_.peers.end() ? nullptr : &*it;
}

std::chrono::milliseconds Forwarder::backoff(std::uint32_t attempts) const {
  auto wait = options_.first_retry;
  for (std::uint32_t i = 1; i < attempts && wait < options_.max_retry; ++i) {
    wait *= 2;
  }
  return std::min(wait, options_.max_retry);
}

Result<void> Forwarder::push(const common::TransactionId& id) {
  CFD_TRY(tx, service_.outgoing(id));
  const auto* peer = find_peer(tx.target_server);
  if (peer == nullptr) {
    return fail(ErrCode::unknown_destination,
                std::format("server '{}' is not a configured peer", tx.target_server));
  }

  CFD_TRY(client, net::HttpClient::create(peer->url, {
                                                         .ca_file = options_.ca_file,
                                                         .io_timeout = options_.io_timeout,
                                                         .user_agent = "confide-forwarder",
                                                     }));
  client.set_token(peer->token);

  // stop() cancels the client while it is registered here.
  {
    const std::scoped_lock lock(mu_);
    if (stop_) {
      return fail(ErrCode::unavailable, "forwarder is stopping");
    }
    active_ = &client;
  }
  struct Unregister {
    Forwarder& self;
    ~Unregister() {
      const std::scoped_lock lock(self.mu_);
      self.active_ = nullptr;
    }
  } const unregister{*this};

  const std::string target = std::string{kInternal} + "/" + id.str();
  const auto created = client.call(http_verb::post, kInternal,
                                   api::encode_receive(transaction::ReceiveRequest{
                                       .id = tx.id,
                                       .sender = tx.sender,
                                       .target_user = tx.target_user,
                                       .created_at = tx.created_at,
                                       .expires_at = tx.expires_at,
                                       .comment = tx.comment,
                                   }));
  bool committed_there = false;
  if (!created) {
    if (created.error().code != ErrCode::conflict) {
      return std::unexpected(created.error());
    }
    // An earlier attempt got this far: open there = send the files again
    // (they replace what arrived), committed there = only the
    // confirmation got lost.
    CFD_TRY(body, client.call(http_verb::get, target));
    CFD_TRY(state, state_of(body));
    committed_there = state != "open";
  }

  if (!committed_there) {
    for (const auto& file : tx.files) {
      CFD_TRY(source, store_.open_read(id, file.path));
      auto sent = client.upload(
          target + "/files/" + net::url_encode_path(file.path.str()),
          net::BodySource{
              .size = file.size,
              .read = [&source](std::span<std::byte> buf) { return source.read(buf); },
          },
          file.sha256);
      if (!sent) {
        return std::unexpected(Error::make(
            sent.error().code, std::format("{}: {}", file.path.str(), sent.error().detail)));
      }
    }
    CFD_TRYV(client.call(http_verb::post, target + "/commit"));
  }

  CFD_TRYV(service_.mark_delivered(id));
  tlog_.info(kTag, "{} delivered to {}@{}: {} file(s), {} bytes", id.str(), tx.target_user,
             peer->name, tx.files.size(), tx.total_bytes());
  return {};
}

void Forwarder::run() {
  tlog_.info(kTag, "started, {} peer(s)", options_.peers.size());
  std::unique_lock lock(mu_);
  while (!stop_) {
    if (queue_.empty()) {
      cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      continue;
    }
    const auto next = std::ranges::min_element(queue_, {}, &Pending::due);
    if (const auto due = next->due; due > std::chrono::steady_clock::now()) {
      // Also woken by enqueue() and stop().
      cv_.wait_until(lock, due);
      continue;
    }
    auto item = *next;
    queue_.erase(next);
    lock.unlock();

    const auto res = push(item.id);
    if (!res) {
      const auto& err = res.error();
      if (is_transient(err.code)) {
        ++item.attempts;
        const auto wait = backoff(item.attempts);
        item.due = std::chrono::steady_clock::now() + wait;
        tlog_.warn(kTag, "{} attempt {} failed, retry in {}s: {}", item.id.str(), item.attempts,
                   std::chrono::duration_cast<std::chrono::seconds>(wait).count(), describe(err));
        lock.lock();
        if (!stop_) {
          queue_.push_back(std::move(item));
        }
        continue;
      }
      tlog_.error(kTag, "{} not forwarded, stays committed until the next start: {}", item.id.str(),
                  describe(err));
    }
    lock.lock();
  }
  tlog_.info(kTag, "stopped");
}

}  // namespace confide::forwarder
