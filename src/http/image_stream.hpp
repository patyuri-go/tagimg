#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <drogon/HttpResponse.h>

#include "common/config.hpp"
#include "common/storage.hpp"
#include "db/tagimg_repository.hpp"
#include "image/image_processor.hpp"

namespace tagimg {

class ConnectionRegistry;
class RequestContext;

// GET /api/v1/image/{imageId} 的请求参数。
struct ImageStreamOptions {
  std::string image_id;
  // 客户端传入的尺寸预设；handle() 会结合 download 解析成 preset。
  std::string size = "regular";
  bool download = false;
  ImageSizePreset preset = ImageSizePreset::Regular;
  std::optional<std::string> request_id;
  // 客户端带来的 If-None-Match，命中时返回 304。
  std::optional<std::string> if_none_match;
  // 请求生命周期上下文；为空时 handle() 会创建一个无连接的上下文。
  std::shared_ptr<RequestContext> context;
};

// 图片流式下载端点。
//
// size=original 时用 Drogon 的异步流逐块转发原始字节；其他预设会通过
// VipsSource 流式读取源图，再用 VipsTarget 把 AVIF 直接写入响应流。
// 数据库查询和转码都在固定 worker 线程完成，不占用 IO 线程，也不做
// 服务端转码缓存。
//
// 失败时返回真实 HTTP 状态码加 JSON 错误体；一旦响应头发出，中途出错
// 只能断开连接（chunked 响应的固有限制）。
class ImageStreamer {
public:
  ImageStreamer(std::shared_ptr<const TagimgRepository> images,
                StorageConfig storage,
                ImageStreamMode mode = ImageStreamMode::Proxy,
                ImageProcessingConfig processing = {},
                std::filesystem::path work_dir = {},
                std::shared_ptr<ConnectionRegistry> registry = {});

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
