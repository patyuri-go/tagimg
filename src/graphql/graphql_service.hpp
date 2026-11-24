#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include "ai/clip_embedder.hpp"
#include "db/tagimg_repository.hpp"
#include "job/service_runtime.hpp"

namespace tagimg {

class ConnectionRegistry;

// GraphQL 入口（POST /graphql）。
//
// 请求体是标准的 GraphQL over HTTP 结构：
//   {"query": "...", "operationName": "...", "variables": {...}}
// 传输层错误用 400/413/415，GraphQL 语法/校验/执行错误一律 HTTP 200，
// 放在响应体的 errors 里。
class GraphQLService {
public:
  GraphQLService(std::shared_ptr<ServiceRuntime> runtime,
                 std::shared_ptr<const TagimgRepository> tagimg_repository,
                 std::shared_ptr<ClipEmbedder> clip_embedder,
                 std::shared_ptr<ConnectionRegistry> connection_registry = {},
                 std::vector<std::string> default_excluded_tag_ids = {});

  ~GraphQLService();

  GraphQLService(const GraphQLService &) = delete;
  GraphQLService &operator=(const GraphQLService &) = delete;
  GraphQLService(GraphQLService &&) = delete;
  GraphQLService &operator=(GraphQLService &&) = delete;

  void
  handle(const drogon::HttpRequestPtr &request,
         std::function<void(const drogon::HttpResponsePtr &)> &&callback) const;

  // 停止 worker：取消排队中的请求并 join 所有 worker 线程。可重复调用。
  void shutdown();

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace tagimg
