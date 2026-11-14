#pragma once

#include <functional>
#include <memory>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

namespace tagimg {

class ScanService;

// HTTP 接口层。
//
// 只暴露两个端点：
//   POST /api/v1         统一 RPC 入口，通过 operation 分发
//   GET  /api/v1/schema  返回支持的操作与字段描述
//
// addArtwork 会提交到共用的 ScanService，handler 立即返回；job 完成后
// 再通过回调把响应交回 Drogon IO 线程。
class ApiService {
public:
  explicit ApiService(std::shared_ptr<ScanService> scan_service);

  ~ApiService();

  ApiService(const ApiService &) = delete;
  ApiService &operator=(const ApiService &) = delete;
  ApiService(ApiService &&) = delete;
  ApiService &operator=(ApiService &&) = delete;

  void handle_rpc(
      const drogon::HttpRequestPtr &request,
      std::function<void(const drogon::HttpResponsePtr &)> &&callback) const;

  void handle_schema(
      const drogon::HttpRequestPtr &request,
      std::function<void(const drogon::HttpResponsePtr &)> &&callback) const;

  // 停止接收新任务；未完成的 job 会被丢弃。
  void shutdown();

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace tagimg
