#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <trantor/net/TcpConnection.h>

namespace tagimg {

// 请求被取消的原因。第一个生效的原因会一直保留到请求结束。
enum class CancelReason {
  None,
  ClientClosed,
  Timeout,
  Shutdown,
  Explicit,
};

[[nodiscard]] std::string_view to_string(CancelReason reason) noexcept;

// 一次 HTTP 请求的生命周期上下文。
//
// 由处理入口创建，图片流与 GraphQL worker 共享同一个 shared_ptr。取消是
// 合作式的：cancel() 只置位并通知已注册的回调，下游在检查点自行退出。
//
// 取消回调在 cancel() 的调用线程上同步执行，因此不能在持有队列锁时调用
// cancel()。回调由 context 持有到 context 析构，不提供单独注销接口，避免
// 在回调执行期间销毁 std::stop_callback。
class RequestContext : public std::enable_shared_from_this<RequestContext> {
public:
  explicit RequestContext(
      std::weak_ptr<trantor::TcpConnection> connection = {});
  ~RequestContext();

  RequestContext(const RequestContext &) = delete;
  RequestContext &operator=(const RequestContext &) = delete;
  RequestContext(RequestContext &&) = delete;
  RequestContext &operator=(RequestContext &&) = delete;

  // 线程安全、幂等。返回 true 表示本次调用是第一个取消者。
  bool cancel(CancelReason reason);

  // 注册取消回调。若注册时已经取消，会同步调用一次。
  void add_cancel_handler(std::function<void(CancelReason)> handler);

  [[nodiscard]] bool stop_requested() const noexcept;
  [[nodiscard]] CancelReason reason() const noexcept;
  [[nodiscard]] std::stop_token stop_token() const noexcept;

  // 检查点使用：除已置位的取消原因外，还会确认底层连接仍然存活；连接已
  // 断开时按 ClientClosed 取消。返回 true 表示调用方应立即退出。
  // 会触发取消回调，因此不能在持有队列锁时调用。
  [[nodiscard]] bool cancelled();

  // 连接已经关闭（或连接指针已失效）时返回 false。
  [[nodiscard]] bool connection_alive() const;
  [[nodiscard]] std::shared_ptr<trantor::TcpConnection> connection() const;

private:
  std::stop_source source_;
  std::atomic<CancelReason> reason_{CancelReason::None};
  std::weak_ptr<trantor::TcpConnection> connection_;
  // 构造时是否关联了连接；没有连接的上下文不做连接存活检查。
  bool has_connection_ = false;

  // 回调对象必须由某个长期存活的对象持有；这里用 vector 保存到 context
  // 析构，注册时不在锁内构造（已停止的 stop_callback 会同步回调）。
  std::mutex handlers_mutex_;
  std::vector<std::unique_ptr<std::stop_callback<std::function<void()>>>>
      handlers_;
};

// 按 TCP 连接维护活动请求，用于在连接断开时批量取消。
class ConnectionRegistry {
public:
  void register_context(const std::shared_ptr<RequestContext> &context);
  void unregister_context(const std::shared_ptr<RequestContext> &context);

  // 连接断开：取消并移除该连接上的所有上下文。
  void cancel_connection(const trantor::TcpConnectionPtr &connection);

  // 服务关停：取消并清空所有上下文。
  void cancel_all(CancelReason reason);

private:
  std::mutex mutex_;
  std::unordered_map<const trantor::TcpConnection *,
                     std::unordered_set<std::shared_ptr<RequestContext>>>
      contexts_;
};

} // namespace tagimg
