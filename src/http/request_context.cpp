#include "http/request_context.hpp"

#include <utility>

namespace tagimg {

std::string_view to_string(CancelReason reason) noexcept {
  switch (reason) {
  case CancelReason::None:
    return "none";
  case CancelReason::ClientClosed:
    return "client_closed";
  case CancelReason::Timeout:
    return "timeout";
  case CancelReason::Shutdown:
    return "shutdown";
  case CancelReason::Explicit:
    return "explicit";
  }
  return "unknown";
}

RequestContext::RequestContext(
    std::weak_ptr<trantor::TcpConnection> connection)
    : connection_(std::move(connection)) {
  has_connection_ = !connection_.expired();
}

RequestContext::~RequestContext() = default;

bool RequestContext::cancel(CancelReason reason) {
  if (reason == CancelReason::None) {
    return false;
  }

  // 让 context 在回调派发期间保持存活：回调可能释放最后一个 session/job，
  // 从而释放 context 的最后一个外部引用。
  auto self = shared_from_this();

  CancelReason expected = CancelReason::None;
  if (!reason_.compare_exchange_strong(expected, reason,
                                       std::memory_order_acq_rel)) {
    return false;
  }
  source_.request_stop();
  return true;
}

void RequestContext::add_cancel_handler(
    std::function<void(CancelReason)> handler) {
  if (!handler) {
    return;
  }

  // 已停止时 stop_callback 的构造会同步执行回调；因此构造必须在锁外，
  // 回调里可能去拿队列锁。对象构造完成后再交给 context 持有。
  auto callback = std::make_unique<std::stop_callback<std::function<void()>>>(
      source_.get_token(), [this, handler = std::move(handler)] {
        handler(reason_.load(std::memory_order_acquire));
      });

  std::lock_guard lock(handlers_mutex_);
  handlers_.push_back(std::move(callback));
}

bool RequestContext::stop_requested() const noexcept {
  return reason_.load(std::memory_order_acquire) != CancelReason::None;
}

CancelReason RequestContext::reason() const noexcept {
  return reason_.load(std::memory_order_acquire);
}

std::stop_token RequestContext::stop_token() const noexcept {
  return source_.get_token();
}

bool RequestContext::cancelled() {
  if (stop_requested()) {
    return true;
  }
  if (!has_connection_ || connection_alive()) {
    return false;
  }
  // 连接已断开但断开回调尚未送达（或不属于 registry 管理范围）：这里
  // 补一次 ClientClosed，保证检查点一定能看到取消。
  cancel(CancelReason::ClientClosed);
  return true;
}

bool RequestContext::connection_alive() const {
  auto connection = connection_.lock();
  return connection != nullptr && connection->connected();
}

std::shared_ptr<trantor::TcpConnection> RequestContext::connection() const {
  return connection_.lock();
}

void ConnectionRegistry::register_context(
    const std::shared_ptr<RequestContext> &context) {
  if (!context) {
    return;
  }
  auto connection = context->connection();
  if (!connection) {
    return;
  }

  bool disconnected = false;
  {
    std::lock_guard lock(mutex_);
    auto &sessions = contexts_[connection.get()];
    sessions.insert(context);
    // 注册和断开可能并发：插入后复查，已断开就立即撤销登记。
    if (!connection->connected()) {
      sessions.erase(context);
      if (sessions.empty()) {
        contexts_.erase(connection.get());
      }
      disconnected = true;
    }
  }
  if (disconnected) {
    context->cancel(CancelReason::ClientClosed);
  }
}

void ConnectionRegistry::unregister_context(
    const std::shared_ptr<RequestContext> &context) {
  if (!context) {
    return;
  }
  auto connection = context->connection();
  if (!connection) {
    return;
  }

  std::lock_guard lock(mutex_);
  auto found = contexts_.find(connection.get());
  if (found == contexts_.end()) {
    return;
  }
  found->second.erase(context);
  if (found->second.empty()) {
    contexts_.erase(found);
  }
}

void ConnectionRegistry::cancel_connection(
    const trantor::TcpConnectionPtr &connection) {
  if (!connection) {
    return;
  }

  std::unordered_set<std::shared_ptr<RequestContext>> contexts;
  {
    std::lock_guard lock(mutex_);
    auto found = contexts_.find(connection.get());
    if (found == contexts_.end()) {
      return;
    }
    contexts = std::move(found->second);
    contexts_.erase(found);
  }

  // 取消回调可能拿队列锁，必须在 registry 锁之外执行。
  for (const auto &context : contexts) {
    context->cancel(CancelReason::ClientClosed);
  }
}

void ConnectionRegistry::cancel_all(CancelReason reason) {
  std::unordered_map<const trantor::TcpConnection *,
                     std::unordered_set<std::shared_ptr<RequestContext>>>
      contexts;
  {
    std::lock_guard lock(mutex_);
    contexts.swap(contexts_);
  }

  for (const auto &[connection, sessions] : contexts) {
    (void)connection;
    for (const auto &context : sessions) {
      context->cancel(reason);
    }
  }
}

} // namespace tagimg
