#include "http/image_stream.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <drogon/HttpAppFramework.h>
#include <trantor/net/EventLoop.h>
#include <vips/vips.h>

#include "common/uuid.hpp"
#include "http/json_helpers.hpp"
#include "io/byte_stream.hpp"
#include "io/temp_file.hpp"

namespace tagimg {
namespace {

using Clock = std::chrono::steady_clock;

// 每次从字节流读取并发送的块大小。
constexpr std::size_t kChunkBytes = 64u * 1024u;
// 与 ByteStreamOptions 的默认上限保持一致。
constexpr std::size_t kMaxImageBytes = 50u * 1024u * 1024u;
// 响应对象迟迟没有开始发送（例如客户端在响应前断开）时释放已打开的流。
constexpr double kStartTimeoutSeconds = 30.0;
constexpr auto kShutdownTimeout = std::chrono::seconds(2);

struct VipsImageDeleter {
  void operator()(VipsImage *image) const noexcept {
    if (image != nullptr) {
      g_object_unref(image);
    }
  }
};

struct VipsSourceDeleter {
  void operator()(VipsSource *source) const noexcept {
    if (source != nullptr) {
      g_object_unref(source);
    }
  }
};

using VipsImagePtr = std::shared_ptr<VipsImage>;
using VipsSourcePtr = std::shared_ptr<VipsSource>;
using TempFilePtr = std::shared_ptr<TempFile>;

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

std::string make_etag(std::string_view image_id, ImageSizePreset preset,
                      const ImageProcessingConfig &processing) {
  std::string value = '"' + std::string(image_id) + '-' +
                      std::string(to_string(preset));
  if (preset != ImageSizePreset::Original) {
    value += "-q";
    value += std::to_string(processing.quality);
    value += "-s";
    value += std::to_string(processing.speed);
  }
  value += '"';
  return value;
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

// 去掉 Content-Type 的参数部分和首尾空白，得到纯媒体类型。
std::string media_type(std::string_view content_type) {
  const std::size_t semicolon = content_type.find(';');
  std::string_view value = content_type.substr(0, semicolon);
  while (!value.empty() &&
         (value.front() == ' ' || value.front() == '\t' ||
          value.front() == '\r' || value.front() == '\n')) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         (value.back() == ' ' || value.back() == '\t' ||
          value.back() == '\r' || value.back() == '\n')) {
    value.remove_suffix(1);
  }
  return std::string(value);
}

void log_vips_error(std::string_view context) {
  const char *message = vips_error_buffer();
  std::cerr << "image streamer: " << context;
  if (message != nullptr && *message != '\0') {
    std::cerr << ": " << message;
  }
  std::cerr << '\n';
  vips_error_clear();
}

// 只有这些格式的多页结果才代表动图；多页 TIFF 等仍然按静态图缩略。
bool mime_can_be_animated(std::string_view mime_type) {
  return mime_type == "image/gif" || mime_type == "image/webp" ||
         mime_type == "image/png" || mime_type == "image/avif" ||
         mime_type == "image/heic" || mime_type == "image/heif";
}

// 判断图片是否是真正的多帧图片。page-height 用于 GIF/WebP 等把多帧
// 拼接成一张长图的加载结果；n-pages 则直接表达页数。
bool image_is_animated(VipsImage *image, std::string_view mime_type) {
  if (!mime_can_be_animated(mime_type)) {
    return false;
  }
  const int pages = vips_image_get_n_pages(image);
  if (pages > 1) {
    return true;
  }
  const int page_height = vips_image_get_page_height(image);
  return page_height > 0 && vips_image_get_height(image) > page_height;
}

struct SourceContext {
  std::shared_ptr<HttpByteStream> stream;
  std::atomic<bool> *stop = nullptr;
  bool failed = false;
};

void destroy_source_context(gpointer data, GClosure *) {
  delete static_cast<SourceContext *>(data);
}

gint64 source_read(VipsSourceCustom *, void *buffer, gint64 length,
                   gpointer user_data) {
  auto *context = static_cast<SourceContext *>(user_data);
  if (length <= 0) {
    return 0;
  }
  if (context->stop != nullptr &&
      context->stop->load(std::memory_order_relaxed)) {
    context->failed = true;
    return -1;
  }
  const auto requested = static_cast<std::size_t>(length);
  auto read = context->stream->read(
      std::span<std::byte>(static_cast<std::byte *>(buffer), requested));
  if (!read) {
    context->failed = true;
    return -1;
  }
  return static_cast<gint64>(*read);
}

struct TargetContext {
  // 不拥有流；ResponseStream 由 pump_avif 的栈上 unique_ptr 管理。
  drogon::ResponseStream *stream = nullptr;
  bool failed = false;
};

void destroy_target_context(gpointer data, GClosure *) {
  delete static_cast<TargetContext *>(data);
}

gint64 target_write(VipsTargetCustom *, const void *data, gint64 length,
                    gpointer user_data) {
  auto *context = static_cast<TargetContext *>(user_data);
  if (length <= 0) {
    return 0;
  }
  if (context->failed || !context->stream) {
    return -1;
  }
  const auto size = static_cast<std::size_t>(length);
  const std::string chunk(static_cast<const char *>(data), size);
  if (!context->stream->send(chunk)) {
    context->failed = true;
    return -1;
  }
  return length;
}

// 简单的超时看门狗：析构时取消并 join，避免线程泄漏。
class TimeoutGuard {
public:
  TimeoutGuard(std::chrono::steady_clock::duration remaining,
               std::function<void()> on_timeout)
      : on_timeout_(std::move(on_timeout)) {
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
      timed_out_.store(true, std::memory_order_relaxed);
      if (on_timeout_) {
        on_timeout_();
      }
      return;
    }
    thread_ = std::thread([this, remaining] {
      std::unique_lock lock(mutex_);
      if (!cv_.wait_for(lock, remaining, [this] { return done_; })) {
        timed_out_.store(true, std::memory_order_relaxed);
        lock.unlock();
        if (on_timeout_) {
          on_timeout_();
        }
      }
    });
  }

  ~TimeoutGuard() {
    {
      std::lock_guard lock(mutex_);
      done_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  TimeoutGuard(const TimeoutGuard &) = delete;
  TimeoutGuard &operator=(const TimeoutGuard &) = delete;

  [[nodiscard]] bool timed_out() const noexcept {
    return timed_out_.load(std::memory_order_relaxed);
  }

private:
  std::function<void()> on_timeout_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool done_ = false;
  std::atomic<bool> timed_out_{false};
  std::thread thread_;
};

} // namespace

// 一条进行中的下载。stream/image/source 只在 Impl::mutex 保护下读写。
struct ImageStreamer::Impl : std::enable_shared_from_this<ImageStreamer::Impl> {
  struct StreamSession {
    std::string image_id;
    std::shared_ptr<HttpByteStream> stream;
    VipsImagePtr image;
    VipsSourcePtr source;
    std::atomic<bool> stop{false};
    std::atomic<bool> timed_out{false};
    // pump 是否已经开始；用于回收「客户端在响应前断开」的会话。
    std::atomic<bool> started{false};
    // 是否占用了转码并发槽位；在 Impl::mutex 保护下读写，由 finish() 释放。
    bool encode_slot = false;
  };

  enum class ProcessResult {
    Done,
    Retry,
    Fallback,
    TimedOut,
  };

  Impl(std::shared_ptr<const TagimgRepository> image_repository,
       StorageConfig storage_config, ImageStreamMode stream_mode,
       ImageProcessingConfig processing_config,
       std::filesystem::path work_dir_value)
      : images(std::move(image_repository)), storage(std::move(storage_config)),
        mode(stream_mode), processing(processing_config),
        work_dir(std::move(work_dir_value)) {
    const std::uint32_t count =
        processing.max_concurrency == 0 ? 1 : processing.max_concurrency;
    encode_limit = count;
    workers.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      workers.emplace_back([this] { worker_loop(); });
    }
  }

  ~Impl() {
    shutdown();
  }

  std::shared_ptr<const TagimgRepository> images;
  StorageConfig storage;
  ImageStreamMode mode = ImageStreamMode::Proxy;
  ImageProcessingConfig processing;
  std::filesystem::path work_dir;

  mutable std::mutex mutex;
  std::condition_variable queue_cv;
  std::condition_variable idle;
  std::deque<std::function<void()>> tasks;
  std::vector<std::thread> workers;
  std::unordered_set<std::shared_ptr<StreamSession>> active;
  bool stopping = false;

  // 转码并发槽位：worker 只负责定位/探测，真正的 AVIF 编码在 pump 线程
  // 里执行。用这里的计数把同时运行的编码限制在 max_concurrency。
  std::condition_variable encode_cv;
  std::size_t encode_active = 0;
  std::size_t encode_limit = 1;

  void worker_loop() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock lock(mutex);
        queue_cv.wait(lock, [this] { return stopping || !tasks.empty(); });
        if (stopping && tasks.empty()) {
          return;
        }
        task = std::move(tasks.front());
        tasks.pop_front();
      }
      task();
    }
  }

  void handle(ImageStreamOptions options,
              std::function<void(const drogon::HttpResponsePtr &)> &&callback);

  void resolve(const std::shared_ptr<StreamSession> &session,
               ImageStreamOptions options,
               std::function<void(const drogon::HttpResponsePtr &)> callback);

  ProcessResult process_streaming(
      const std::shared_ptr<StreamSession> &session, const std::string &url,
      const std::string &mime_type, const ImageStreamOptions &options,
      const std::string &etag, Clock::time_point deadline,
      std::function<void(const drogon::HttpResponsePtr &)> callback);

  ProcessResult process_temp(
      const std::shared_ptr<StreamSession> &session, const std::string &url,
      const std::string &mime_type, const ImageStreamOptions &options,
      const std::string &etag, Clock::time_point deadline,
      std::function<void(const drogon::HttpResponsePtr &)> callback);

  void respond_ok(
      const std::shared_ptr<StreamSession> &session,
      const std::string &mime_type, const ImageStreamOptions &options,
      std::string etag,
      const std::function<void(const drogon::HttpResponsePtr &)> &callback);

  void respond_temp_ok(
      const std::shared_ptr<StreamSession> &session, TempFilePtr temp,
      const std::string &mime_type, const ImageStreamOptions &options,
      std::string etag,
      const std::function<void(const drogon::HttpResponsePtr &)> &callback);

  // 在 IO 线程构造流式 AVIF 响应，随后在独立 pump 线程里编码。
  void respond_avif_stream(
      const std::shared_ptr<StreamSession> &session, VipsImagePtr image,
      VipsSourcePtr source, TempFilePtr temp, ImageStreamOptions options,
      std::string etag, Clock::time_point deadline,
      const std::function<void(const drogon::HttpResponsePtr &)> &callback);

  // 302 模式下把上游内容地址交给客户端直连。
  void respond_redirect(
      const std::shared_ptr<StreamSession> &session, std::string url,
      const std::function<void(const drogon::HttpResponsePtr &)> &callback);

  // 在 IO 线程构造错误响应；可以安全地从工作线程调用。
  void
  fail(const std::shared_ptr<StreamSession> &session,
       const ImageStreamOptions &options, drogon::HttpStatusCode status,
       std::string code, std::string message, bool retryable,
       const std::function<void(const drogon::HttpResponsePtr &)> &callback);

  void pump(const std::shared_ptr<StreamSession> &session,
            drogon::ResponseStreamPtr stream);

  void pump_temp(const std::shared_ptr<StreamSession> &session, TempFilePtr temp,
                 drogon::ResponseStreamPtr stream);

  void pump_avif(const std::shared_ptr<StreamSession> &session,
                 VipsImagePtr image, VipsSourcePtr source, TempFilePtr temp,
                 ImageStreamOptions options, Clock::time_point deadline,
                 drogon::ResponseStreamPtr stream);

  void finish(const std::shared_ptr<StreamSession> &session);

  void shutdown();

  void post(std::function<void()> task) {
    drogon::app().getLoop()->runInLoop(std::move(task));
  }

  std::shared_ptr<HttpByteStream> session_stream(
      const std::shared_ptr<StreamSession> &session) {
    std::lock_guard lock(mutex);
    return session->stream;
  }

  VipsImagePtr session_image(const std::shared_ptr<StreamSession> &session) {
    std::lock_guard lock(mutex);
    return session->image;
  }

  void reset_session_image(const std::shared_ptr<StreamSession> &session) {
    std::lock_guard lock(mutex);
    session->image.reset();
    session->source.reset();
  }

  // 在 IO 线程构造响应之前先占一个转码槽位，避免同时跑起任意多个 aom 编码。
  // 排队等待时间也计入本次任务超时；槽位由 finish() 统一释放。
  bool acquire_encode_slot(const std::shared_ptr<StreamSession> &session,
                           Clock::time_point deadline) {
    std::unique_lock lock(mutex);
    for (;;) {
      if (stopping || session->stop.load(std::memory_order_relaxed)) {
        return false;
      }
      if (encode_active < encode_limit) {
        ++encode_active;
        session->encode_slot = true;
        return true;
      }
      if (Clock::now() >= deadline) {
        session->timed_out.store(true, std::memory_order_relaxed);
        return false;
      }
      encode_cv.wait_until(lock, deadline);
    }
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

  // download 永远返回原图；否则按 size 选择预设，缺省 regular。
  if (options.download) {
    options.preset = ImageSizePreset::Original;
  } else {
    const std::string_view requested =
        options.size.empty() ? std::string_view("regular")
                             : std::string_view(options.size);
    const auto parsed = parse_image_preset(requested);
    if (!parsed) {
      auto body = envelope(
          options.request_id, nullptr,
          single_error(error_object(
              "INVALID_ARGUMENT",
              "size must be one of mini, thumb, small, regular, original",
              Json(), {{"retryable", false}})));
      callback(json_response(std::move(body), drogon::k400BadRequest));
      return;
    }
    options.preset = *parsed;
  }

  auto self = shared_from_this();
  auto session = std::make_shared<StreamSession>();
  session->image_id = options.image_id;
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
    active.insert(session);
    tasks.push_back(
        [self, session, options = std::move(options),
         callback = std::move(callback)]() mutable {
          self->resolve(session, std::move(options), std::move(callback));
        });
  }
  queue_cv.notify_one();
}

void ImageStreamer::Impl::resolve(
    const std::shared_ptr<StreamSession> &session, ImageStreamOptions options,
    std::function<void(const drogon::HttpResponsePtr &)> callback) {
  const auto deadline =
      Clock::now() +
      std::chrono::seconds(processing.timeout_seconds == 0
                               ? 1
                               : processing.timeout_seconds);

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

  // 上游内容端点直接按实体 ID 取原始字节，不再查实体库。
  const std::string url = entity_content_url(storage, **entity_id);
  const bool original = options.preset == ImageSizePreset::Original;
  // 只有内联原图走 302；download 和需要转码的预设都由服务端代理。
  if (mode == ImageStreamMode::Redirect && original && !options.download) {
    auto self = shared_from_this();
    post([self, session, url, callback = std::move(callback)]() mutable {
      self->respond_redirect(session, url, callback);
    });
    return;
  }

  // 只有 302 不经过服务端处理；其余响应都适用 ETag/304。
  const bool proxied =
      !(mode == ImageStreamMode::Redirect && original && !options.download);
  const std::string etag =
      make_etag(session->image_id, options.preset, processing);
  if (proxied && options.if_none_match &&
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

  auto opened = HttpByteStream::open(url);
  if (!opened) {
    const bool too_large = opened.error() == ByteStreamError::TooLarge;
    fail(session, options,
         too_large ? drogon::k413RequestEntityTooLarge
                   : drogon::k503ServiceUnavailable,
         too_large ? "IMAGE_TOO_LARGE" : "STORAGE_UNAVAILABLE",
         too_large ? "image exceeds the size limit"
                   : "storage service is unavailable",
         !too_large, callback);
    return;
  }

  // 确认上游真的开始返回数据；HTTP 状态码与响应头到这里才可知。
  const auto started = (*opened)->wait_started();
  if (!started) {
    if (started.error() == ByteStreamError::TooLarge) {
      fail(session, options, drogon::k413RequestEntityTooLarge,
           "IMAGE_TOO_LARGE", "image exceeds the size limit", false, callback);
      return;
    }
    if ((*opened)->http_status() == 404) {
      fail(session, options, drogon::k404NotFound, "STORAGE_NOT_FOUND",
           "entity content not found", false, callback);
      return;
    }
    fail(session, options, drogon::k503ServiceUnavailable,
         "STORAGE_UNAVAILABLE", "storage service is unavailable", true,
         callback);
    return;
  }

  auto source = std::shared_ptr<HttpByteStream>(std::move(*opened));
  const std::string mime_type = media_type(source->content_type());
  if (!mime_type.empty() && !is_image_mime(mime_type)) {
    fail(session, options, drogon::k502BadGateway, "STORAGE_INVALID_RESPONSE",
         "storage service did not return an image", false, callback);
    return;
  }
  const auto content_length = source->content_length();
  if (content_length && *content_length > kMaxImageBytes) {
    fail(session, options, drogon::k413RequestEntityTooLarge, "IMAGE_TOO_LARGE",
         "image exceeds the size limit", false, callback);
    return;
  }

  {
    std::lock_guard lock(mutex);
    session->stream = source;
  }
  if (session->stop.load(std::memory_order_relaxed)) {
    finish(session);
    return;
  }

  if (original) {
    auto self = shared_from_this();
    post([self, session, mime_type, options, etag,
          callback = std::move(callback)]() mutable {
      self->respond_ok(session, mime_type, options, std::move(etag), callback);
    });
    return;
  }

  auto result = process_streaming(session, url, mime_type, options, etag,
                                  deadline, callback);
  if (result == ProcessResult::Fallback) {
    result = process_temp(session, url, mime_type, options, etag, deadline,
                          callback);
  }
  if (result == ProcessResult::Retry) {
    fail(session, options, drogon::k503ServiceUnavailable,
         "STORAGE_UNAVAILABLE", "storage service is unavailable", true,
         callback);
    return;
  }
  if (result == ProcessResult::TimedOut) {
    if (session->timed_out.load(std::memory_order_relaxed)) {
      fail(session, options, drogon::k504GatewayTimeout,
           "IMAGE_PROCESSING_TIMEOUT", "image processing timed out", false,
           callback);
    } else {
      finish(session);
    }
  }
}

ImageStreamer::Impl::ProcessResult ImageStreamer::Impl::process_streaming(
    const std::shared_ptr<StreamSession> &session, const std::string &url,
    const std::string &mime_type, const ImageStreamOptions &options,
    const std::string &etag, Clock::time_point deadline,
    std::function<void(const drogon::HttpResponsePtr &)> callback) {
  if (session->stop.load(std::memory_order_relaxed) ||
      Clock::now() >= deadline) {
    session->timed_out.store(Clock::now() >= deadline,
                             std::memory_order_relaxed);
    return ProcessResult::TimedOut;
  }

  VipsSource *raw_source = VIPS_SOURCE(vips_source_custom_new());
  if (raw_source == nullptr) {
    return ProcessResult::Fallback;
  }
  auto *context = new SourceContext();
  context->stream = session_stream(session);
  context->stop = &session->stop;
  if (context->stream == nullptr) {
    delete context;
    g_object_unref(raw_source);
    return ProcessResult::Retry;
  }
  g_signal_connect_data(raw_source, "read", G_CALLBACK(source_read), context,
                        destroy_source_context, G_CONNECT_DEFAULT);
  VipsSourcePtr source(raw_source, VipsSourceDeleter{});

  VipsImage *raw_image =
      vips_image_new_from_source(source.get(), "",
                                 "access", VIPS_ACCESS_SEQUENTIAL, nullptr);
  if (raw_image == nullptr) {
    log_vips_error("sequential source probe failed");
    if (session->stop.load(std::memory_order_relaxed) ||
        Clock::now() >= deadline) {
      session->timed_out.store(true, std::memory_order_relaxed);
      return ProcessResult::TimedOut;
    }
    return ProcessResult::Fallback;
  }
  VipsImagePtr image(raw_image, VipsImageDeleter{});

  const int width = vips_image_get_width(image.get());
  const int height = vips_image_get_height(image.get());
  if (width <= 0 || height <= 0) {
    return ProcessResult::Fallback;
  }

  if (image_is_animated(image.get(), mime_type)) {
    reset_session_image(session);
    std::shared_ptr<HttpByteStream> old = session_stream(session);
    {
      std::lock_guard lock(mutex);
      session->stream.reset();
    }
    if (old) {
      old->cancel();
    }

    if (mode == ImageStreamMode::Redirect) {
      auto self = shared_from_this();
      post([self, session, url, callback = std::move(callback)]() mutable {
        self->respond_redirect(session, url, callback);
      });
      return ProcessResult::Done;
    }

    auto opened = HttpByteStream::open(url);
    if (!opened) {
      return ProcessResult::Retry;
    }
    if (!(*opened)->wait_started()) {
      return ProcessResult::Retry;
    }
    auto stream = std::shared_ptr<HttpByteStream>(std::move(*opened));
    {
      std::lock_guard lock(mutex);
      session->stream = stream;
    }
    auto self = shared_from_this();
    post([self, session, mime_type, options, etag,
          callback = std::move(callback)]() mutable {
      self->respond_ok(session, mime_type, options, etag, callback);
    });
    return ProcessResult::Done;
  }

  const std::uint64_t pixels = static_cast<std::uint64_t>(width) *
                               static_cast<std::uint64_t>(height);
  if (pixels > processing.max_pixels) {
    fail(session, options, drogon::k413RequestEntityTooLarge, "IMAGE_TOO_LARGE",
         "image exceeds the pixel limit", false, callback);
    return ProcessResult::Done;
  }

  {
    std::lock_guard lock(mutex);
    session->image = image;
    session->source = source;
  }

  if (!acquire_encode_slot(session, deadline)) {
    if (session->timed_out.load(std::memory_order_relaxed)) {
      fail(session, options, drogon::k504GatewayTimeout,
           "IMAGE_PROCESSING_TIMEOUT", "image processing timed out", false,
           callback);
    } else {
      finish(session);
    }
    return ProcessResult::Done;
  }

  auto self = shared_from_this();
  post([self, session, image, source, temp = TempFilePtr(),
        options = options, etag = etag, deadline,
        callback = std::move(callback)]() mutable {
    self->respond_avif_stream(session, image, source, temp, options, etag,
                              deadline, callback);
  });
  return ProcessResult::Done;
}

ImageStreamer::Impl::ProcessResult ImageStreamer::Impl::process_temp(
    const std::shared_ptr<StreamSession> &session, const std::string &url,
    const std::string &mime_type, const ImageStreamOptions &options,
    const std::string &etag, Clock::time_point deadline,
    std::function<void(const drogon::HttpResponsePtr &)> callback) {
  if (session->stop.load(std::memory_order_relaxed) ||
      Clock::now() >= deadline) {
    session->timed_out.store(Clock::now() >= deadline,
                             std::memory_order_relaxed);
    return ProcessResult::TimedOut;
  }

  reset_session_image(session);
  std::shared_ptr<HttpByteStream> old = session_stream(session);
  {
    std::lock_guard lock(mutex);
    session->stream.reset();
  }
  if (old) {
    old->cancel();
  }

  auto opened = HttpByteStream::open(url);
  if (!opened) {
    return ProcessResult::Retry;
  }
  if (!(*opened)->wait_started()) {
    return ProcessResult::Retry;
  }
  auto source = std::shared_ptr<HttpByteStream>(std::move(*opened));
  {
    std::lock_guard lock(mutex);
    session->stream = source;
  }

  auto file_result = TempFile::create(work_dir, ".img");
  if (!file_result) {
    {
      std::lock_guard lock(mutex);
      session->stream.reset();
    }
    source->cancel();
    fail(session, options, drogon::k500InternalServerError,
         "IMAGE_PROCESSING_FAILED", "could not create temp file", false,
         callback);
    return ProcessResult::Done;
  }
  auto temp = std::make_shared<TempFile>(std::move(*file_result));

  std::vector<std::byte> buffer(kChunkBytes);
  for (;;) {
    if (session->stop.load(std::memory_order_relaxed) ||
        Clock::now() >= deadline) {
      session->timed_out.store(Clock::now() >= deadline,
                               std::memory_order_relaxed);
      {
        std::lock_guard lock(mutex);
        session->stream.reset();
      }
      source->cancel();
      return ProcessResult::TimedOut;
    }
    const auto read = source->read(buffer);
    if (!read) {
      {
        std::lock_guard lock(mutex);
        session->stream.reset();
      }
      source->cancel();
      return ProcessResult::Retry;
    }
    if (*read == 0) {
      break;
    }
    const auto write = temp->write(
        std::span<const std::byte>(buffer.data(), *read));
    if (!write) {
      {
        std::lock_guard lock(mutex);
        session->stream.reset();
      }
      source->cancel();
      fail(session, options, drogon::k500InternalServerError,
           "IMAGE_PROCESSING_FAILED", "could not write temp file", false,
           callback);
      return ProcessResult::Done;
    }
  }
  if (temp->size() == 0) {
    {
      std::lock_guard lock(mutex);
      session->stream.reset();
    }
    source->cancel();
    return ProcessResult::Retry;
  }
  if (!temp->close()) {
    {
      std::lock_guard lock(mutex);
      session->stream.reset();
    }
    source->cancel();
    fail(session, options, drogon::k500InternalServerError,
         "IMAGE_PROCESSING_FAILED", "could not close temp file", false,
         callback);
    return ProcessResult::Done;
  }
  {
    std::lock_guard lock(mutex);
    session->stream.reset();
  }
  source.reset();

  VipsImage *raw_image = vips_image_new_from_file(
      temp->path().c_str(), "access", VIPS_ACCESS_SEQUENTIAL, nullptr);
  if (raw_image == nullptr) {
    log_vips_error("temp file decode failed");
    fail(session, options, drogon::k500InternalServerError,
         "IMAGE_PROCESSING_FAILED",
         "image could not be decoded or encoded as avif", false, callback);
    return ProcessResult::Done;
  }
  VipsImagePtr image(raw_image, VipsImageDeleter{});

  const int width = vips_image_get_width(image.get());
  const int height = vips_image_get_height(image.get());
  if (width <= 0 || height <= 0) {
    fail(session, options, drogon::k500InternalServerError,
         "IMAGE_PROCESSING_FAILED",
         "image could not be decoded or encoded as avif", false, callback);
    return ProcessResult::Done;
  }

  if (image_is_animated(image.get(), mime_type)) {
    if (mode == ImageStreamMode::Redirect) {
      auto self = shared_from_this();
      post([self, session, url, callback = std::move(callback)]() mutable {
        self->respond_redirect(session, url, callback);
      });
      return ProcessResult::Done;
    }
    auto self = shared_from_this();
    post([self, session, temp, mime_type, options, etag,
          callback = std::move(callback)]() mutable {
      self->respond_temp_ok(session, temp, mime_type, options, etag, callback);
    });
    return ProcessResult::Done;
  }

  const std::uint64_t pixels = static_cast<std::uint64_t>(width) *
                               static_cast<std::uint64_t>(height);
  if (pixels > processing.max_pixels) {
    fail(session, options, drogon::k413RequestEntityTooLarge, "IMAGE_TOO_LARGE",
         "image exceeds the pixel limit", false, callback);
    return ProcessResult::Done;
  }

  {
    std::lock_guard lock(mutex);
    session->image = image;
    session->source.reset();
  }

  if (!acquire_encode_slot(session, deadline)) {
    if (session->timed_out.load(std::memory_order_relaxed)) {
      fail(session, options, drogon::k504GatewayTimeout,
           "IMAGE_PROCESSING_TIMEOUT", "image processing timed out", false,
           callback);
    } else {
      finish(session);
    }
    return ProcessResult::Done;
  }

  auto self = shared_from_this();
  post([self, session, image, temp, options = options, etag = etag, deadline,
        callback = std::move(callback)]() mutable {
    self->respond_avif_stream(session, image, VipsSourcePtr(), temp, options,
                              etag, deadline, callback);
  });
  return ProcessResult::Done;
}

void ImageStreamer::Impl::respond_ok(
    const std::shared_ptr<StreamSession> &session,
    const std::string &mime_type, const ImageStreamOptions &options,
    std::string etag,
    const std::function<void(const drogon::HttpResponsePtr &)> &callback) {
  const std::string content_type =
      mime_type.empty() ? "application/octet-stream" : mime_type;

  auto self = shared_from_this();
  auto response = drogon::HttpResponse::newAsyncStreamResponse(
      [self, session](drogon::ResponseStreamPtr stream) {
        self->pump(session, std::move(stream));
      });
  response->setStatusCode(drogon::k200OK);
  response->setContentTypeString(content_type);
  response->addHeader("Content-Disposition",
                      content_disposition(session->image_id, mime_type,
                                          options.download));
  response->addHeader("ETag", etag);
  response->addHeader("Cache-Control", "private, max-age=3600");
  response->addHeader("X-Content-Type-Options", "nosniff");

  // 客户端在响应发出前就断开时，异步回调不会被执行，用定时器兜底回收。
  drogon::app().getLoop()->runAfter(kStartTimeoutSeconds, [self, session] {
    bool expected = false;
    if (!session->started.compare_exchange_strong(expected, true)) {
      return;
    }
    session->stop.store(true, std::memory_order_relaxed);
    auto source = self->session_stream(session);
    if (source) {
      source->cancel();
    }
    self->finish(session);
  });

  callback(response);
}

void ImageStreamer::Impl::respond_temp_ok(
    const std::shared_ptr<StreamSession> &session, TempFilePtr temp,
    const std::string &mime_type, const ImageStreamOptions &options,
    std::string etag,
    const std::function<void(const drogon::HttpResponsePtr &)> &callback) {
  const std::string content_type =
      mime_type.empty() ? "application/octet-stream" : mime_type;

  auto self = shared_from_this();
  auto response = drogon::HttpResponse::newAsyncStreamResponse(
      [self, session, temp](drogon::ResponseStreamPtr stream) {
        self->pump_temp(session, temp, std::move(stream));
      });
  response->setStatusCode(drogon::k200OK);
  response->setContentTypeString(content_type);
  response->addHeader("Content-Disposition",
                      content_disposition(session->image_id, mime_type,
                                          options.download));
  response->addHeader("ETag", etag);
  response->addHeader("Cache-Control", "private, max-age=3600");
  response->addHeader("X-Content-Type-Options", "nosniff");

  drogon::app().getLoop()->runAfter(kStartTimeoutSeconds, [self, session] {
    bool expected = false;
    if (!session->started.compare_exchange_strong(expected, true)) {
      return;
    }
    session->stop.store(true, std::memory_order_relaxed);
    self->finish(session);
  });

  callback(response);
}

void ImageStreamer::Impl::respond_avif_stream(
    const std::shared_ptr<StreamSession> &session, VipsImagePtr image,
    VipsSourcePtr source, TempFilePtr temp, ImageStreamOptions options,
    std::string etag, Clock::time_point deadline,
    const std::function<void(const drogon::HttpResponsePtr &)> &callback) {
  auto self = shared_from_this();
  auto response = drogon::HttpResponse::newAsyncStreamResponse(
      [self, session, image, source, temp, options, deadline](
          drogon::ResponseStreamPtr stream) mutable {
        self->pump_avif(session, image, source, temp, options, deadline,
                        std::move(stream));
      });
  response->setStatusCode(drogon::k200OK);
  response->setContentTypeString("image/avif");
  response->addHeader("Content-Disposition",
                      content_disposition(session->image_id, "image/avif",
                                          options.download));
  response->addHeader("ETag", etag);
  response->addHeader("Cache-Control", "private, max-age=3600");
  response->addHeader("X-Content-Type-Options", "nosniff");

  drogon::app().getLoop()->runAfter(kStartTimeoutSeconds, [self, session] {
    bool expected = false;
    if (!session->started.compare_exchange_strong(expected, true)) {
      return;
    }
    session->stop.store(true, std::memory_order_relaxed);
    auto source = self->session_stream(session);
    if (source) {
      source->cancel();
    }
    if (auto image = self->session_image(session)) {
      vips_image_set_kill(image.get(), TRUE);
    }
    self->finish(session);
  });

  callback(response);
}

void ImageStreamer::Impl::respond_redirect(
    const std::shared_ptr<StreamSession> &session, std::string url,
    const std::function<void(const drogon::HttpResponsePtr &)> &callback) {
  post([url = std::move(url), callback]() mutable {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k302Found);
    response->addHeader("Location", url);
    // 上游内容地址可能随配置变化，不缓存，避免客户端拿到过期地址。
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

  auto source = session_stream(session);

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

void ImageStreamer::Impl::pump_temp(
    const std::shared_ptr<StreamSession> &session, TempFilePtr temp,
    drogon::ResponseStreamPtr stream) {
  session->started.store(true, std::memory_order_relaxed);

  auto self = shared_from_this();
  std::thread([self, session, temp = std::move(temp),
               stream = std::move(stream)]() mutable {
    std::ifstream file(temp->path(), std::ios::binary);
    if (!file) {
      stream->close();
      self->finish(session);
      return;
    }
    std::array<char, kChunkBytes> buffer{};
    while (file && !session->stop.load(std::memory_order_relaxed)) {
      file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const auto count = file.gcount();
      if (count <= 0) {
        break;
      }
      if (!stream->send(std::string(buffer.data(),
                                    static_cast<std::size_t>(count)))) {
        break;
      }
    }
    stream->close();
    stream.reset();
    self->finish(session);
  }).detach();
}

void ImageStreamer::Impl::pump_avif(
    const std::shared_ptr<StreamSession> &session, VipsImagePtr image,
    VipsSourcePtr source, TempFilePtr temp, ImageStreamOptions options,
    Clock::time_point deadline, drogon::ResponseStreamPtr stream) {
  session->started.store(true, std::memory_order_relaxed);

  const std::string suffix = avif_save_options(processing);
  auto self = shared_from_this();
  if (session->stop.load(std::memory_order_relaxed)) {
    stream->close();
    self->finish(session);
    return;
  }
  std::thread([self, session, image = std::move(image),
               source = std::move(source), temp = std::move(temp),
               options = std::move(options), deadline, suffix,
               stream = std::move(stream)]() mutable {
    const auto now = Clock::now();
    if (now >= deadline) {
      session->timed_out.store(true, std::memory_order_relaxed);
      stream->close();
      self->finish(session);
      return;
    }

    TimeoutGuard guard(deadline - now, [self, session, image] {
      session->stop.store(true, std::memory_order_relaxed);
      session->timed_out.store(true, std::memory_order_relaxed);
      auto stream = self->session_stream(session);
      if (stream) {
        stream->cancel();
      }
      if (image) {
        vips_image_set_kill(image.get(), TRUE);
      }
    });

    const int dimension = preset_dimension(options.preset);
    VipsImage *thumbnail_raw = nullptr;
    if (dimension <= 0 ||
        vips_thumbnail_image(image.get(), &thumbnail_raw, dimension, "height",
                             dimension, "size", VIPS_SIZE_DOWN, "auto_rotate",
                             TRUE, nullptr) != 0) {
      log_vips_error("thumbnail failed");
      stream->close();
      self->finish(session);
      return;
    }
    VipsImagePtr thumbnail(thumbnail_raw, VipsImageDeleter{});

    VipsTarget *target = VIPS_TARGET(vips_target_custom_new());
    if (target == nullptr) {
      log_vips_error("target creation failed");
      stream->close();
      self->finish(session);
      return;
    }
    auto *context = new TargetContext();
    context->stream = stream.get();
    g_signal_connect_data(target, "write", G_CALLBACK(target_write), context,
                          destroy_target_context, G_CONNECT_DEFAULT);

    const int result = vips_image_write_to_target(
        thumbnail.get(), suffix.c_str(), target, nullptr);
    if (result != 0) {
      log_vips_error("avif encoding failed");
    }
    g_object_unref(target);
    stream->close();
    stream.reset();
    self->finish(session);
  }).detach();
}

void ImageStreamer::Impl::finish(
    const std::shared_ptr<StreamSession> &session) {
  bool removed = false;
  bool slot_released = false;
  {
    std::lock_guard lock(mutex);
    if (session->encode_slot) {
      session->encode_slot = false;
      if (encode_active > 0) {
        --encode_active;
      }
      slot_released = true;
    }
    removed = active.erase(session) > 0;
  }
  if (removed) {
    idle.notify_all();
  }
  if (slot_released) {
    encode_cv.notify_one();
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
    tasks.clear();
  }
  queue_cv.notify_all();
  encode_cv.notify_all();

  for (const auto &session : sessions) {
    session->stop.store(true, std::memory_order_relaxed);
    auto source = session_stream(session);
    if (source) {
      // 唤醒阻塞在 read()/wait_started() 的工作线程。
      source->cancel();
    }
    if (auto image = session_image(session)) {
      vips_image_set_kill(image.get(), TRUE);
    }
    // 响应还没开始发送的会话不会再有 pump 线程来收尾（典型情况是客户端在
    // 收到响应头之前就断开），这里直接回收，避免关停时白等到超时。
    if (!session->started.load(std::memory_order_relaxed)) {
      finish(session);
    }
  }

  for (auto &worker : workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }

  std::unique_lock lock(mutex);
  if (!idle.wait_for(lock, kShutdownTimeout,
                     [this] { return active.empty(); })) {
    std::cerr << "image streamer: " << active.size()
              << " stream(s) still active after shutdown timeout\n";
  }
}

ImageStreamer::ImageStreamer(std::shared_ptr<const TagimgRepository> images,
                             StorageConfig storage, ImageStreamMode mode,
                             ImageProcessingConfig processing,
                             std::filesystem::path work_dir)
    : impl_(std::make_shared<Impl>(std::move(images), std::move(storage), mode,
                                   processing, std::move(work_dir))) {
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