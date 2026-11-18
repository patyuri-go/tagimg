#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include <drogon/HttpClient.h>
#include <trantor/net/EventLoopThread.h>

#include "common/result.hpp"

namespace tagimg {

enum class AiHealthError {
  // 连不上、超时，或服务返回非 2xx（例如 tagger 未加载时的 503）。
  Unavailable,
  Timeout,
  // 200 但响应体不是预期的 /health JSON。
  InvalidResponse,
};

[[nodiscard]] std::string_view to_string(AiHealthError error) noexcept;

// /health 暴露的模型状态。
struct AiHealthStatus {
  bool tagger_loaded = false;
  bool clip_loaded = false;

  [[nodiscard]] bool ready() const noexcept {
    return tagger_loaded && clip_loaded;
  }
};

using AiHealthResult = Result<AiHealthStatus, AiHealthError>;

// AI sidecar 的 /health 客户端。
//
// Python 端在 tagger 未加载时返回 503；tagger 已加载时返回 200 和
// {"loaded": true, "clip": {"loaded": <bool>}}。扫描任务要求 tagger 与
// CLIP 同时就绪，因此这里把两者都解析出来。
class AiHealthClient {
public:
  explicit AiHealthClient(
      std::string endpoint,
      std::chrono::milliseconds timeout = std::chrono::seconds(1));

  AiHealthClient(const AiHealthClient &) = delete;
  AiHealthClient &operator=(const AiHealthClient &) = delete;
  AiHealthClient(AiHealthClient &&) = delete;
  AiHealthClient &operator=(AiHealthClient &&) = delete;

  [[nodiscard]] AiHealthResult check() const;

private:
  std::string endpoint_;
  std::chrono::milliseconds timeout_;
  trantor::EventLoopThread loop_thread_;
  drogon::HttpClientPtr client_;
};

} // namespace tagimg
