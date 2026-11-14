#pragma once

#include <functional>
#include <memory>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include "db/tagimg_repository.hpp"
#include "job/scan_service.hpp"

namespace tagimg {

// GraphQL 入口（POST /graphql）。
//
// 请求体是标准的 GraphQL over HTTP 结构：
//   {"query": "...", "operationName": "...", "variables": {...}}
// 传输层错误用 400/413/415，GraphQL 语法/校验/执行错误一律 HTTP 200，
// 放在响应体的 errors 里。
class GraphQLService {
public:
  GraphQLService(std::shared_ptr<ScanService> scan_service,
                 std::shared_ptr<const TagimgRepository> tagimg_repository);

  ~GraphQLService();

  GraphQLService(const GraphQLService &) = delete;
  GraphQLService &operator=(const GraphQLService &) = delete;
  GraphQLService(GraphQLService &&) = delete;
  GraphQLService &operator=(GraphQLService &&) = delete;

  void
  handle(const drogon::HttpRequestPtr &request,
         std::function<void(const drogon::HttpResponsePtr &)> &&callback) const;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace tagimg
