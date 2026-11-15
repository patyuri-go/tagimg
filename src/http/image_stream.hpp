#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <drogon/HttpResponse.h>

#include "db/entity_repository.hpp"
#include "db/tagimg_repository.hpp"

namespace tagimg {

// GET /api/v1/image/{imageId} 的请求参数。
struct ImageStreamOptions {
  std::string image_id;
  // 只从指定存储节点读取；为空表示按实体库返回的顺序依次尝试。
  std::string storage_id;
  bool download = false;
  std::optional<std::string> request_id;
  // 客户端带来的 If-None-Match，命中时返回 304。
  std::optional<std::string> if_none_match;
};

// 图片流式下载端点。
//
// 响应体是实体图片的原始字节，用 Drogon 的异步流逐块发出：数据库查询和
// 建立连接在独立线程完成，之后由另一个线程持续 read() 并 send()，因此
// 既不会把整张图放进内存，也不会长时间占用 IO 线程。
//
// 失败时返回真实 HTTP 状态码加 JSON 错误体；一旦响应头发出，中途出错
// 只能断开连接（chunked 响应的固有限制）。
class ImageStreamer {
public:
  ImageStreamer(std::shared_ptr<const EntityRepository> entities,
                std::shared_ptr<const TagimgRepository> images);

  ~ImageStreamer();

  ImageStreamer(const ImageStreamer &) = delete;
  ImageStreamer &operator=(const ImageStreamer &) = delete;
  ImageStreamer(ImageStreamer &&) = delete;
  ImageStreamer &operator=(ImageStreamer &&) = delete;

  // 必须在 Drogon IO 线程调用。
  void handle(ImageStreamOptions options,
              std::function<void(const drogon::HttpResponsePtr &)> &&callback);

  // 中断所有进行中的传输，最多等待 2 秒；可以重复调用。
  void shutdown();

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace tagimg
