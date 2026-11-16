#include "http/image_stream.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <drogon/HttpAppFramework.h>
#include <trantor/net/EventLoop.h>

#include "common/uuid.hpp"
#include "http/json_helpers.hpp"
#include "io/byte_stream.hpp"

namespace tagimg {
namespace {

// 每次从字节流读取并发送的块大小。
constexpr std::size_t kChunkBytes = 64u * 1024u;
// 与 ByteStreamOptions 的默认上限保持一致。
constexpr std::size_t kMaxImageBytes = 50u * 1024u * 1024u;
// 响应对象迟迟没有开始发送（例如客户端在响应前断开）时释放已打开的流。
constexpr double kStartTimeoutSeconds = 30.0;
constexpr auto kShutdownTimeout = std::chrono::seconds(2);

std::string_view extension_for(std::string_view mime_type) {
  if (mime_type == "image/jpeg" || mime_type == "image/jpg") {
    return "jpg";
  }
  if (mime_type == "image/png") {
    return "png";
  }
  if (mime_type == "image/webp") {
    return "webp";
  }
  if (mime_type == "image/gif") {
    return "gif";
  }
  if (mime_type == "image/avif") {
    return "avif";
  }
  if (mime_type == "image/bmp") {
    return "bmp";
  }
  if (mime_type == "image/tiff") {
    return "tiff";
  }
  return {};
}

std::string content_disposition(std::string_view image_id,
                                std::string_view mime_type, bool download) {
  std::string value =
      download ? "attachment; filename=\"" : "inline; filename=\"";
  value += image_id;
  const std::string_view extension = extension_for(mime_type);
  if (!extension.empty()) {
    value += '.';
    value += extension;
  }
  value += '"';
  return value;
}

std::string make_etag(std::string_view image_id, std::uint64_t size) {
  return '"' + std::string(image_id) + '-' + std::to_string(size) + '"';
}

// 只做精确比较：按逗号拆分、去掉空白与 W/ 前缀，* 视为命中。
bool etag_matches(std::string_view header, std::string_view etag) {
  std::size_t start = 0;
  while (start <= header.size()) {
    const std::size_t comma = header.find(',', start);
    std::string_view item = header.substr(start, comma == std::string_view::npos
                                                     ? std::string_view::npos
                                                     : comma - start);
    while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) {
      item.remove_prefix(1);
    }
    while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) {
      item.remove_suffix(1);
    }
    if (item.rfind("W/", 0) == 0) {
      item.remove_prefix(2);
    }
    if (item == "*" || item == etag) {
      return true;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return false;
}

bool is_image_mime(std::string_view mime_type) {
  return mime_type.rfind("image/", 0) == 0;
}

} // namespace

// 一条进行中的下载。字段 stream 只在 Impl::mutex 保护下读写。
struct ImageStreamer::Impl : std::enable_shared_from_this<ImageStreamer::Impl> {
  struct StreamSession {
    std::string image_id;
    std::shared_ptr<WebdavByteStream> stream;
    std::atomic<bool> stop{false};
    // pump 是否已经开始；用于回收「客户端在响应前断开」的会话。
    std::atomic<bool> started{false};
  };

  Impl(std::shared_ptr<const EntityRepository> entities_repository,
       std::shared_ptr<const TagimgRepository> image_repository,
       ImageStreamMode stream_mode)
      : entities(std::move(entities_repository)),
        images(std::move(image_repository)), mode(stream_mode) {
  }

  std::shared_ptr<const EntityRepository> entities;
  std::shared_ptr<const TagimgRepository> images;
  ImageStreamMode mode = ImageStreamMode::Proxy;

  mutable std::mutex mutex;
  std::condition_variable idle;
  std::unordered_set<std::shared_ptr<StreamSession>> active;
  bool stopping = false;

  void handle(ImageStreamOptions options,
              std::function<void(const drogon::HttpResponsePtr &)> &&callback);

  void resolve(const std::shared_ptr<StreamSession> &session,
               ImageStreamOptions options,
               std::function<void(const drogon::HttpResponsePtr &)> callback);

  void respond_ok(
      const std::shared_ptr<StreamSession> &session,
      const EntityMetadata &metadata, const ImageStreamOptions &options,
      const std::function<void(const drogon::HttpResponsePtr &)> &callback);

  // 302 模式下把解析出的直链交给客户端。
  void respond_redirect(
      const std::shared_ptr<StreamSession> &session, std::string location,
      const std::function<void(const drogon::HttpResponsePtr &)> &callback);

  // 在 IO 线程构造错误响应；可以安全地从工作线程调用。
  void
  fail(const std::shared_ptr<StreamSession> &session,
       const ImageStreamOptions &options, drogon::HttpStatusCode status,
       std::string code, std::string message, bool retryable,
       const std::function<void(const drogon::HttpResponsePtr &)> &callback);

  // 在 IO 线程启动 pump 线程。
  void pump(const std::shared_ptr<StreamSession> &session,
            drogon::ResponseStreamPtr stream);

  void finish(const std::shared_ptr<StreamSession> &session);

  void shutdown();

  void post(std::function<void()> task) {
    drogon::app().getLoop()->runInLoop(std::move(task));
  }
};

void ImageStreamer::Impl::handle(
    ImageStreamOptions options,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
  if (!is_uuid(options.image_id)) {
    auto body = envelope(options.request_id, nullptr,
                         single_error(error_object(
                             "INVALID_ARGUMENT", "image id is not a valid uuid",
                             Json(), {{"retryable", false}})));
    callback(json_response(std::move(body), drogon::k400BadRequest));
    return;
  }

  if (!options.storage_id.empty() && !is_uuid(options.storage_id)) {
    auto body =
        envelope(options.request_id, nullptr,
                 single_error(error_object("INVALID_ARGUMENT",
                                           "storageId is not a valid uuid",
                                           Json(), {{"retryable", false}})));
    callback(json_response(std::move(body), drogon::k400BadRequest));
    return;
  }

  {
    std::lock_guard lock(mutex);
    if (stopping) {
      auto body = envelope(options.request_id, nullptr,
                           single_error(error_object(
                               "SERVICE_UNAVAILABLE", "server is shutting down",
                               Json(), {{"retryable", true}})));
      callback(json_response(std::move(body), drogon::k503ServiceUnavailable));
      return;
    }
  }

  auto session = std::make_shared<StreamSession>();
  session->image_id = options.image_id;
  {
    std::lock_guard lock(mutex);
    active.insert(session);
  }

  auto self = shared_from_this();
  std::thread([self, session, options = std::move(options),
               callback = std::move(callback)]() mutable {
    self->resolve(session, std::move(options), std::move(callback));
  }).detach();
}

void ImageStreamer::Impl::resolve(
    const std::shared_ptr<StreamSession> &session, ImageStreamOptions options,
    std::function<void(const drogon::HttpResponsePtr &)> callback) {
  // 前端只拿得到图片 ID，这里先换成实体 ID 再走实体库的存储定位。
  const auto entity_id = images->find_image_entity_id(session->image_id);
  if (!entity_id) {
    if (entity_id.error() == TagimgRepositoryError::InvalidArgument) {
      fail(session, options, drogon::k400BadRequest, "INVALID_ARGUMENT",
           "image id is not a valid uuid", false, callback);
      return;
    }
    fail(session, options, drogon::k503ServiceUnavailable, "REPOSITORY_FAILED",
         "image repository query failed", true, callback);
    return;
  }
  // 图片存在但没有 entity_id 时无法定位原始文件，按不存在处理。
  if (!*entity_id) {
    fail(session, options, drogon::k404NotFound, "IMAGE_NOT_FOUND",
         "image not found", false, callback);
    return;
  }

  const auto metadata = entities->find_entity(**entity_id);
  if (!metadata) {
    switch (metadata.error()) {
    case EntityRepositoryError::NotFound:
      fail(session, options, drogon::k404NotFound, "ENTITY_NOT_FOUND",
           "entity not found", false, callback);
      return;
    case EntityRepositoryError::InvalidEntityId:
      fail(session, options, drogon::k400BadRequest, "INVALID_ARGUMENT",
           "entity id is not a valid uuid", false, callback);
      return;
    case EntityRepositoryError::ConnectionFailed:
    case EntityRepositoryError::QueryFailed:
      fail(session, options, drogon::k503ServiceUnavailable,
           "REPOSITORY_FAILED", "entity repository query failed", true,
           callback);
      return;
    }
    fail(session, options, drogon::k503ServiceUnavailable, "REPOSITORY_FAILED",
         "entity repository query failed", true, callback);
    return;
  }

  if (!is_image_mime(metadata->mime_type)) {
    fail(session, options, drogon::k415UnsupportedMediaType, "NOT_IMAGE",
         "entity is not an image", false, callback);
    return;
  }

  // 超过单流上限时直接拒绝，避免传输到一半才中断。
  if (metadata->size > kMaxImageBytes) {
    fail(session, options, drogon::k413RequestEntityTooLarge, "IMAGE_TOO_LARGE",
         "image exceeds the size limit", false, callback);
    return;
  }

  const std::string etag = make_etag(session->image_id, metadata->size);
  // 302 模式不返回图片字节，也不发 ETag，避免客户端缓存住短期直链。
  if (mode == ImageStreamMode::Proxy && options.if_none_match &&
      etag_matches(*options.if_none_match, etag)) {
    post([callback, etag]() mutable {
      auto response = drogon::HttpResponse::newHttpResponse();
      response->setStatusCode(drogon::k304NotModified);
      response->addHeader("ETag", etag);
      response->addHeader("Cache-Control", "private, max-age=3600");
      callback(response);
    });
    finish(session);
    return;
  }

  if (session->stop.load(std::memory_order_relaxed)) {
    finish(session);
    return;
  }

  const auto locations = entities->find_webdav_locations(**entity_id);
  if (!locations) {
    if (locations.error() == EntityRepositoryError::NotFound) {
      fail(session, options, drogon::k404NotFound, "STORAGE_NOT_FOUND",
           "no available storage location", false, callback);
    } else {
      fail(session, options, drogon::k503ServiceUnavailable,
           "REPOSITORY_FAILED", "entity repository query failed", true,
           callback);
    }
    return;
  }

  bool matched_storage = false;
  std::optional<ByteStreamError> last_error;
  for (const WebdavLocation &location : *locations) {
    if (!options.storage_id.empty() &&
        location.storage_id != options.storage_id) {
      continue;
    }
    matched_storage = true;
    if (session->stop.load(std::memory_order_relaxed)) {
      finish(session);
      return;
    }

    if (mode == ImageStreamMode::Redirect) {
      // 先跟随上游全部重定向拿到最终直链，再让客户端直接访问。
      const auto resolved = resolve_redirect_url(location.url);
      if (!resolved) {
        continue;
      }
      if (session->stop.load(std::memory_order_relaxed)) {
        finish(session);
        return;
      }
      auto self = shared_from_this();
      post([self, session, location = *resolved,
            callback = std::move(callback)]() mutable {
        self->respond_redirect(session, std::move(location), callback);
      });
      return;
    }

    auto opened = WebdavByteStream::open(location.url);
    if (!opened) {
      last_error = opened.error();
      continue;
    }
    // 确认节点真的开始返回数据，否则换下一个位置。
    const auto started = (*opened)->wait_started();
    if (!started) {
      last_error = started.error();
      continue;
    }

    {
      std::lock_guard lock(mutex);
      session->stream = std::shared_ptr<WebdavByteStream>(std::move(*opened));
    }
    if (session->stop.load(std::memory_order_relaxed)) {
      finish(session);
      return;
    }

    auto self = shared_from_this();
    post([self, session, metadata = *metadata, options,
          callback = std::move(callback)]() mutable {
      self->respond_ok(session, metadata, options, callback);
    });
    return;
  }

  if (!matched_storage) {
    fail(session, options, drogon::k404NotFound, "STORAGE_NOT_FOUND",
         "no available storage location", false, callback);
    return;
  }

  const bool too_large = last_error == ByteStreamError::TooLarge;
  fail(session, options,
       too_large ? drogon::k413RequestEntityTooLarge
                 : drogon::k503ServiceUnavailable,
       too_large ? "IMAGE_TOO_LARGE" : "STORAGE_UNAVAILABLE",
       too_large ? "image exceeds the size limit"
                 : "all storage locations are unavailable",
       !too_large, callback);
}

void ImageStreamer::Impl::respond_ok(
    const std::shared_ptr<StreamSession> &session,
    const EntityMetadata &metadata, const ImageStreamOptions &options,
    const std::function<void(const drogon::HttpResponsePtr &)> &callback) {
  const std::string content_type = metadata.mime_type.empty()
                                       ? "application/octet-stream"
                                       : metadata.mime_type;

  auto self = shared_from_this();
  auto response = drogon::HttpResponse::newAsyncStreamResponse(
      [self, session](drogon::ResponseStreamPtr stream) {
        self->pump(session, std::move(stream));
      });
  response->setStatusCode(drogon::k200OK);
  response->setContentTypeString(content_type);
  response->addHeader("Content-Disposition",
                      content_disposition(session->image_id, metadata.mime_type,
                                          options.download));
  response->addHeader("ETag", make_etag(session->image_id, metadata.size));
  response->addHeader("Cache-Control", "private, max-age=3600");
  response->addHeader("X-Content-Type-Options", "nosniff");

  // 客户端在响应发出前就断开时，异步回调不会被执行，用定时器兜底回收。
  drogon::app().getLoop()->runAfter(kStartTimeoutSeconds, [self, session] {
    bool expected = false;
    if (!session->started.compare_exchange_strong(expected, true)) {
      return;
    }
    session->stop.store(true, std::memory_order_relaxed);
    std::shared_ptr<WebdavByteStream> source;
    {
      std::lock_guard lock(self->mutex);
      source = session->stream;
    }
    if (source) {
      source->cancel();
    }
    self->finish(session);
  });

  callback(response);
}

void ImageStreamer::Impl::respond_redirect(
    const std::shared_ptr<StreamSession> &session, std::string location,
    const std::function<void(const drogon::HttpResponsePtr &)> &callback) {
  post([location = std::move(location), callback]() mutable {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k302Found);
    response->addHeader("Location", location);
    // 直链可能带有时效签名，不缓存，避免客户端拿到过期地址。
    response->addHeader("Cache-Control", "no-store");
    callback(response);
  });
  finish(session);
}

void ImageStreamer::Impl::fail(
    const std::shared_ptr<StreamSession> &session,
    const ImageStreamOptions &options, drogon::HttpStatusCode status,
    std::string code, std::string message, bool retryable,
    const std::function<void(const drogon::HttpResponsePtr &)> &callback) {
  auto self = shared_from_this();
  post([self, request_id = options.request_id, status, code = std::move(code),
        message = std::move(message), retryable, callback]() mutable {
    Json extensions;
    extensions["retryable"] = retryable;
    auto body =
        envelope(request_id, nullptr,
                 single_error(error_object(std::move(code), std::move(message),
                                           Json(), std::move(extensions))));
    callback(json_response(std::move(body), status));
  });
  finish(session);
}

void ImageStreamer::Impl::pump(const std::shared_ptr<StreamSession> &session,
                               drogon::ResponseStreamPtr stream) {
  session->started.store(true, std::memory_order_relaxed);

  std::shared_ptr<WebdavByteStream> source;
  {
    std::lock_guard lock(mutex);
    source = session->stream;
  }

  auto self = shared_from_this();
  std::thread([self, session, source = std::move(source),
               stream = std::move(stream)]() mutable {
    std::vector<std::byte> buffer(kChunkBytes);
    while (source != nullptr &&
           !session->stop.load(std::memory_order_relaxed)) {
      const auto read = source->read(buffer);
      if (!read || *read == 0) {
        break;
      }
      const std::string chunk(reinterpret_cast<const char *>(buffer.data()),
                              *read);
      if (!stream->send(chunk)) {
        // 客户端已断开。
        break;
      }
    }
    stream->close();
    // 先释放响应流，再析构字节流（析构会中断 curl 并回收线程）。
    stream.reset();
    source.reset();
    self->finish(session);
  }).detach();
}

void ImageStreamer::Impl::finish(
    const std::shared_ptr<StreamSession> &session) {
  bool removed = false;
  {
    std::lock_guard lock(mutex);
    removed = active.erase(session) > 0;
  }
  if (removed) {
    idle.notify_all();
  }
}

void ImageStreamer::Impl::shutdown() {
  std::vector<std::shared_ptr<StreamSession>> sessions;
  {
    std::lock_guard lock(mutex);
    if (stopping) {
      return;
    }
    stopping = true;
    sessions.assign(active.begin(), active.end());
  }

  for (const auto &session : sessions) {
    session->stop.store(true, std::memory_order_relaxed);
    std::shared_ptr<WebdavByteStream> source;
    {
      std::lock_guard lock(mutex);
      source = session->stream;
    }
    if (source) {
      // 唤醒阻塞在 read()/wait_started() 的工作线程。
      source->cancel();
    }
    // 响应还没开始发送的会话不会再有 pump 线程来收尾（典型情况是客户端在
    // 收到响应头之前就断开），这里直接回收，避免关停时白等到超时。
    if (!session->started.load(std::memory_order_relaxed)) {
      finish(session);
    }
  }

  std::unique_lock lock(mutex);
  if (!idle.wait_for(lock, kShutdownTimeout,
                     [this] { return active.empty(); })) {
    std::cerr << "image streamer: " << active.size()
              << " stream(s) still active after shutdown timeout\n";
  }
}

ImageStreamer::ImageStreamer(std::shared_ptr<const EntityRepository> entities,
                             std::shared_ptr<const TagimgRepository> images,
                             ImageStreamMode mode)
    : impl_(std::make_shared<Impl>(std::move(entities), std::move(images),
                                   mode)) {
}

ImageStreamer::~ImageStreamer() {
  shutdown();
}

void ImageStreamer::handle(
    ImageStreamOptions options,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
  impl_->handle(std::move(options), std::move(callback));
}

void ImageStreamer::shutdown() {
  if (!impl_) {
    return;
  }
  impl_->shutdown();
}

} // namespace tagimg
