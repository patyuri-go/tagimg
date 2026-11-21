#include "http/image_stream.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <list>
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

#include "cache/image_cache_keys.hpp"
#include "cache/image_cache_object.hpp"
#include "cache/image_cache_store.hpp"
#include "common/uuid.hpp"
#include "http/json_helpers.hpp"
#include "http/request_context.hpp"
#include "image/animated_avif.hpp"
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

// 存储服务返回的 Content-Type 不一定可信（例如 .jpg 上传的 GIF 会被
// 标成 image/jpeg），所以直接嗅探文件头，用真实格式决定加载方式。
constexpr std::size_t kSniffBytes = 16;

std::string sniff_image_mime(const std::uint8_t *data, std::size_t size) {
  if (size >= 6 && (std::memcmp(data, "GIF87a", 6) == 0 ||
                    std::memcmp(data, "GIF89a", 6) == 0)) {
    return "image/gif";
  }
  if (size >= 12 && std::memcmp(data, "RIFF", 4) == 0 &&
      std::memcmp(data + 8, "WEBP", 4) == 0) {
    return "image/webp";
  }
  if (size >= 8 && std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
    return "image/png";
  }
  if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
    return "image/jpeg";
  }
  if (size >= 12 && std::memcmp(data + 4, "ftyp", 4) == 0) {
    if (std::memcmp(data + 8, "avif", 4) == 0 ||
        std::memcmp(data + 8, "avis", 4) == 0) {
      return "image/avif";
    }
    if (std::memcmp(data + 8, "heic", 4) == 0 ||
        std::memcmp(data + 8, "heix", 4) == 0 ||
        std::memcmp(data + 8, "mif1", 4) == 0 ||
        std::memcmp(data + 8, "msf1", 4) == 0) {
      return "image/heic";
    }
  }
  return {};
}

// libvips 只有部分加载器接受 "n" 参数；pngload 不认 "n"，传了会直接加载失败。
// 这里传入的是嗅探出的真实格式，而不是存储返回的 Content-Type。
bool mime_supports_multipage_load(std::string_view mime_type) {
  return mime_type == "image/gif" || mime_type == "image/webp" ||
         mime_type == "image/avif" || mime_type == "image/heic" ||
         mime_type == "image/heif";
}

// 判断图片是否是真正的多帧图片。page-height 用于 GIF/WebP 等把多帧
// 拼接成一张长图的加载结果；n-pages 则直接表达页数。
bool image_is_animated(VipsImage *image) {
  const int pages = vips_image_get_n_pages(image);
  if (pages > 1) {
    return true;
  }
  const int page_height = vips_image_get_page_height(image);
  return page_height > 0 && vips_image_get_height(image) > page_height;
}

void log_cache_error(std::string_view action, ImageCacheError error) {
  std::cerr << "image cache: " << action << ": " << to_string(error) << '\n';
}

// 源文件下载期间同时写入的缓存对象。写失败只影响缓存，不影响响应。
class SourceCacheState {
public:
  explicit SourceCacheState(std::shared_ptr<StoredImageCacheObject> object)
      : object_(std::move(object)) {
  }

  bool write(std::span<const std::byte> data) {
    std::lock_guard lock(mutex_);
    if (failed_ || finished_ || !object_) {
      return false;
    }
    auto &out = object_->write();
    out.write(reinterpret_cast<const char *>(data.data()),
              static_cast<std::streamsize>(data.size()));
    if (!out.good()) {
      failed_ = true;
      return false;
    }
    return true;
  }

  void mark_complete() {
    std::lock_guard lock(mutex_);
    complete_ = true;
  }

  void set_content_type(std::string_view content_type) {
    std::lock_guard lock(mutex_);
    content_type_ = content_type;
    if (object_) {
      object_->set_content_type(content_type_);
    }
  }

  // 下载完整时 commit 并 pin；返回是否已可作为文件读取。
  bool finish() {
    std::lock_guard lock(mutex_);
    if (finished_) {
      return stored_;
    }
    finished_ = true;
    if (!object_ || failed_ || !complete_) {
      object_.reset();
      return false;
    }
    if (!content_type_.empty()) {
      object_->set_content_type(content_type_);
    }
    const auto committed = object_->commit();
    if (!committed) {
      log_cache_error("source commit failed", committed.error());
      object_.reset();
      return false;
    }
    if (*committed != ImageCacheCommit::Committed) {
      std::cerr << "image cache: source skipped because it exceeds the limit\n";
      object_.reset();
      return false;
    }
    const auto pinned = object_->pin();
    if (!pinned) {
      log_cache_error("source pin failed", pinned.error());
      object_.reset();
      return false;
    }
    stored_ = true;
    return true;
  }

  void discard() {
    std::lock_guard lock(mutex_);
    if (finished_) {
      return;
    }
    finished_ = true;
    object_.reset();
  }

  [[nodiscard]] bool complete() const {
    std::lock_guard lock(mutex_);
    return complete_;
  }

  [[nodiscard]] bool stored() const {
    std::lock_guard lock(mutex_);
    return stored_;
  }

  [[nodiscard]] std::shared_ptr<StoredImageCacheObject> object() const {
    std::lock_guard lock(mutex_);
    return object_;
  }

  [[nodiscard]] std::filesystem::path file_path() const {
    std::lock_guard lock(mutex_);
    return object_ ? object_->file_path() : std::filesystem::path();
  }

private:
  mutable std::mutex mutex_;
  std::shared_ptr<StoredImageCacheObject> object_;
  std::string content_type_;
  bool failed_ = false;
  bool complete_ = false;
  bool finished_ = false;
  bool stored_ = false;
};

struct SourceContext {
  std::shared_ptr<HttpByteStream> stream;
  std::shared_ptr<RequestContext> request;
  std::shared_ptr<SourceCacheState> cache;
  bool failed = false;
  // 加载前预读的文件头，source_read 先吐这部分再继续读流。
  std::vector<std::byte> prefix;
  std::size_t prefix_pos = 0;
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
  if (context->request != nullptr && context->request->cancelled()) {
    context->failed = true;
    return -1;
  }
  const auto requested = static_cast<std::size_t>(length);
  auto *out = static_cast<std::byte *>(buffer);
  std::size_t copied = 0;
  if (context->prefix_pos < context->prefix.size()) {
    const std::size_t available = context->prefix.size() - context->prefix_pos;
    const std::size_t take = std::min(available, requested);
    std::memcpy(out, context->prefix.data() + context->prefix_pos, take);
    context->prefix_pos += take;
    copied = take;
  }
  if (copied == requested) {
    return static_cast<gint64>(copied);
  }
  auto read = context->stream->read(
      std::span<std::byte>(out + copied, requested - copied));
  if (!read) {
    context->failed = true;
    return -1;
  }
  if (*read == 0) {
    if (context->cache) {
      context->cache->mark_complete();
    }
    return static_cast<gint64>(copied);
  }
  if (context->cache) {
    context->cache->write(
        std::span<const std::byte>(out + copied, *read));
  }
  return static_cast<gint64>(copied + *read);
}

struct TargetContext {
  // 不拥有流；ResponseStream 由 pump_avif 的栈上 unique_ptr 管理。
  drogon::ResponseStream *stream = nullptr;
  RequestContext *request = nullptr;
  std::shared_ptr<StoredImageCacheObject> cache;
  bool cache_failed = false;
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
  if (context->request != nullptr && context->request->cancelled()) {
    context->failed = true;
    return -1;
  }
  const auto size = static_cast<std::size_t>(length);
  if (context->cache && !context->cache_failed) {
    auto &out = context->cache->write();
    out.write(static_cast<const char *>(data),
              static_cast<std::streamsize>(size));
    if (!out.good()) {
      context->cache_failed = true;
    }
  }
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

// 图片流队列上限；超过后直接返回 503，避免慢客户端堆积任务。
constexpr std::size_t kQueueLimit = 128;

// 一条进行中的下载。stream/image/source 只在 Impl::mutex 保护下读写。
struct ImageStreamer::Impl : std::enable_shared_from_this<ImageStreamer::Impl> {
  struct StreamSession {
    std::string image_id;
    std::shared_ptr<RequestContext> context;
    std::shared_ptr<HttpByteStream> stream;
    VipsImagePtr image;
    VipsSourcePtr source;
    // 正在读取的缓存条目；pump 结束前一直持有，防止被淘汰。
    std::shared_ptr<StoredImageCacheObject> cache_pin;
    // 当前变体的写缓存对象；成功编码并发送完成后提交。
    std::shared_ptr<StoredImageCacheObject> variant_cache;
    // 源文件下载时同步写入的缓存对象。
    std::shared_ptr<SourceCacheState> source_cache;
    std::atomic<bool> source_cache_active{false};
    // 旧的临时文件兜底；缓存不可用/超限时才会设置。
    std::shared_ptr<TempFile> temp_pin;
    // pump 是否已经开始；用于回收「客户端在响应前断开」的会话。
    std::atomic<bool> started{false};
    // 是否占用了转码并发槽位；在 Impl::mutex 保护下读写，由 finish() 释放。
    bool encode_slot = false;
  };

  // 队列任务保留 session，取消时可以按 session 精确定位并移除。
  struct QueuedTask {
    std::shared_ptr<StreamSession> session;
    std::function<void()> run;
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
       std::filesystem::path work_dir_value,
       std::shared_ptr<ConnectionRegistry> registry_value,
       std::shared_ptr<ImageCacheStore> cache_value)
      : images(std::move(image_repository)), storage(std::move(storage_config)),
        mode(stream_mode), processing(processing_config),
        work_dir(std::move(work_dir_value)),
        registry(std::move(registry_value)),
        image_cache(std::move(cache_value)) {
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
  std::shared_ptr<ConnectionRegistry> registry;
  std::shared_ptr<ImageCacheStore> image_cache;

  mutable std::mutex mutex;
  std::condition_variable queue_cv;
  std::condition_variable idle;
  std::list<QueuedTask> tasks;
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
      QueuedTask task;
      {
        std::unique_lock lock(mutex);
        queue_cv.wait(lock, [this] { return stopping || !tasks.empty(); });
        if (stopping && tasks.empty()) {
          return;
        }
        task = std::move(tasks.front());
        tasks.pop_front();
      }
      task.run();
    }
  }

  // 从队列里移除指定会话；返回是否确实移除。调用方必须已释放 Impl::mutex。
  bool remove_queued(const std::shared_ptr<StreamSession> &session) {
    std::lock_guard lock(mutex);
    const auto before = tasks.size();
    tasks.remove_if([&session](const QueuedTask &task) {
      return task.session == session;
    });
    return tasks.size() != before;
  }

  void handle(ImageStreamOptions options,
              std::function<void(const drogon::HttpResponsePtr &)> &&callback);

  void resolve(const std::shared_ptr<StreamSession> &session,
               ImageStreamOptions options,
               std::function<void(const drogon::HttpResponsePtr &)> callback);

  ProcessResult process_streaming(
      const std::shared_ptr<StreamSession> &session, const std::string &url,
      const ImageStreamOptions &options, const std::string &etag,
      Clock::time_point deadline,
      std::function<void(const drogon::HttpResponsePtr &)> callback);

  ProcessResult process_temp(
      const std::shared_ptr<StreamSession> &session, const std::string &url,
      const ImageStreamOptions &options, const std::string &etag,
      Clock::time_point deadline,
      std::function<void(const drogon::HttpResponsePtr &)> callback);

  // 从已落盘的源缓存文件解码；session->cache_pin 负责保活。
  ProcessResult process_cached_source(
      const std::shared_ptr<StreamSession> &session,
      const std::filesystem::path &path, const ImageStreamOptions &options,
      const std::string &etag, Clock::time_point deadline,
      std::function<void(const drogon::HttpResponsePtr &)> callback);

  // 创建一个源缓存写对象；失败或禁用时返回空。
  std::shared_ptr<SourceCacheState>
  open_source_cache(const std::shared_ptr<StreamSession> &session,
                    std::string_view mime_type);

  // 创建当前请求的变体缓存写对象；失败或禁用时返回空。
  void build_variant_cache(const std::shared_ptr<StreamSession> &session,
                           const ImageStreamOptions &options);

  // 把剩余上游字节读完并提交源缓存；返回是否可作为文件读取。
  bool drain_source_cache(const std::shared_ptr<StreamSession> &session);

  void respond_ok(
      const std::shared_ptr<StreamSession> &session,
      const std::string &mime_type, const ImageStreamOptions &options,
      std::string etag,
      const std::function<void(const drogon::HttpResponsePtr &)> &callback);

  // 命中缓存时直接把文件流给客户端。
  void respond_cache_hit(
      const std::shared_ptr<StreamSession> &session,
      std::shared_ptr<StoredImageCacheObject> object,
      const std::string &content_type, ImageStreamOptions options,
      std::string etag,
      const std::function<void(const drogon::HttpResponsePtr &)> &callback);

  // 在 IO 线程构造流式 AVIF 响应，随后在独立 pump 线程里编码。
  void respond_avif_stream(
      const std::shared_ptr<StreamSession> &session, VipsImagePtr image,
      VipsSourcePtr source, ImageStreamOptions options, std::string etag,
      Clock::time_point deadline, bool animated,
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

  // 请求已取消或超时：Timeout 返回 504，其余原因直接回收、不发送响应。
  void stop_session(
      const std::shared_ptr<StreamSession> &session,
      const ImageStreamOptions &options,
      const std::function<void(const drogon::HttpResponsePtr &)> &callback);

  void pump(const std::shared_ptr<StreamSession> &session,
            drogon::ResponseStreamPtr stream);

  void pump_cache_file(
      const std::shared_ptr<StreamSession> &session,
      std::shared_ptr<StoredImageCacheObject> object,
      drogon::ResponseStreamPtr stream);

  void pump_avif(const std::shared_ptr<StreamSession> &session,
                 VipsImagePtr image, VipsSourcePtr source,
                 ImageStreamOptions options, Clock::time_point deadline,
                 bool animated, drogon::ResponseStreamPtr stream);

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
    bool timed_out = false;
    {
      std::unique_lock lock(mutex);
      for (;;) {
        // 持锁期间只能查询已置位的状态：cancelled() 会触发取消回调，而
        // 回调需要拿这把锁。
        if (stopping || session->context->stop_requested()) {
          return false;
        }
        if (encode_active < encode_limit) {
          ++encode_active;
          session->encode_slot = true;
          return true;
        }
        if (Clock::now() >= deadline) {
          timed_out = true;
          break;
        }
        encode_cv.wait_until(lock, deadline);
      }
    }
    // 不能在持有 mutex 时 cancel()：取消回调会回来拿队列锁。
    if (timed_out) {
      session->context->cancel(CancelReason::Timeout);
    }
    return false;
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
  session->context = std::move(options.context);
  if (!session->context) {
    session->context = std::make_shared<RequestContext>();
  }

  // 先登记再入队：worker 可能在 register 之前就完成请求，后登记会残留。
  if (registry) {
    registry->register_context(session->context);
  }

  bool stopped = false;
  bool queue_full = false;
  {
    std::lock_guard lock(mutex);
    if (stopping) {
      stopped = true;
    } else if (tasks.size() >= kQueueLimit) {
      queue_full = true;
    } else {
      active.insert(session);
      tasks.push_back(QueuedTask{
          session,
          [self, session, options = std::move(options),
           callback = std::move(callback)]() mutable {
            self->resolve(session, std::move(options), std::move(callback));
          }});
    }
  }

  if (stopped) {
    if (registry) {
      registry->unregister_context(session->context);
    }
    auto body = envelope(
        options.request_id, nullptr,
        single_error(error_object("SERVICE_UNAVAILABLE",
                                  "server is shutting down", Json(),
                                  {{"retryable", true}})));
    callback(json_response(std::move(body), drogon::k503ServiceUnavailable));
    return;
  }
  if (queue_full) {
    if (registry) {
      registry->unregister_context(session->context);
    }
    auto body = envelope(
        options.request_id, nullptr,
        single_error(error_object("QUEUE_FULL", "image stream queue is full",
                                  Json(), {{"retryable", true}})));
    callback(json_response(std::move(body), drogon::k503ServiceUnavailable));
    return;
  }

  queue_cv.notify_one();

  // 入队后注册取消回调：取消发生在注册前时，stop_callback 会同步触发，
  // 直接把刚入队的任务移除。
  std::weak_ptr<Impl> weak_self = weak_from_this();
  std::weak_ptr<StreamSession> weak_session = session;
  session->context->add_cancel_handler(
      [weak_self, weak_session](CancelReason reason) {
        auto impl = weak_self.lock();
        auto session = weak_session.lock();
        if (!impl || !session) {
          return;
        }
        // 客户端关闭但源缓存仍在下载时，保留字节流把源文件收完；其余原因
        // （超时、关停）都立即中断。
        if (reason != CancelReason::ClientClosed ||
            !session->source_cache_active.load(std::memory_order_relaxed)) {
          if (auto source = impl->session_stream(session)) {
            source->cancel();
          }
        }
        if (auto image = impl->session_image(session)) {
          vips_image_set_kill(image.get(), TRUE);
        }
        // 可能有任务正在等转码槽位。
        impl->encode_cv.notify_all();
        // 还在排队就直接移除并回收；运行中的任务由 worker 的检查点退出。
        if (impl->remove_queued(session)) {
          impl->finish(session);
        }
      });
}

std::shared_ptr<SourceCacheState> ImageStreamer::Impl::open_source_cache(
    const std::shared_ptr<StreamSession> &session, std::string_view mime_type) {
  if (!image_cache || !image_cache->enabled()) {
    return {};
  }
  auto created =
      image_cache->create(make_source_cache_key(session->image_id, mime_type));
  if (!created) {
    log_cache_error("source create failed", created.error());
    return {};
  }
  auto state = std::make_shared<SourceCacheState>(
      std::make_shared<StoredImageCacheObject>(std::move(*created)));
  if (!mime_type.empty()) {
    state->set_content_type(mime_type);
  }
  session->source_cache = state;
  session->source_cache_active.store(true, std::memory_order_relaxed);
  return state;
}

void ImageStreamer::Impl::build_variant_cache(
    const std::shared_ptr<StreamSession> &session,
    const ImageStreamOptions &options) {
  if (!image_cache || !image_cache->enabled() ||
      options.preset == ImageSizePreset::Original) {
    return;
  }
  auto created = image_cache->create(
      make_variant_cache_key(session->image_id, options.preset, processing));
  if (!created) {
    log_cache_error("variant create failed", created.error());
    return;
  }
  session->variant_cache = std::make_shared<StoredImageCacheObject>(
      std::move(*created));
}

bool ImageStreamer::Impl::drain_source_cache(
    const std::shared_ptr<StreamSession> &session) {
  auto state = session->source_cache;
  if (!state) {
    return false;
  }
  if (!state->complete()) {
    auto stream = session_stream(session);
    if (stream) {
      std::array<std::byte, kChunkBytes> buffer{};
      for (;;) {
        const auto read = stream->read(buffer);
        if (!read) {
          break;
        }
        if (*read == 0) {
          state->mark_complete();
          break;
        }
        state->write(std::span<const std::byte>(buffer.data(), *read));
      }
    }
  }
  const bool stored = state->finish();
  session->source_cache_active.store(false, std::memory_order_relaxed);
  if (stored) {
    session->cache_pin = state->object();
  } else {
    session->cache_pin.reset();
  }
  {
    std::lock_guard lock(mutex);
    session->stream.reset();
  }
  return stored;
}

void ImageStreamer::Impl::resolve(
    const std::shared_ptr<StreamSession> &session, ImageStreamOptions options,
    std::function<void(const drogon::HttpResponsePtr &)> callback) {
  const auto deadline =
      Clock::now() +
      std::chrono::seconds(processing.timeout_seconds == 0
                               ? 1
                               : processing.timeout_seconds);
  const auto stop_if_cancelled = [&]() {
    if (!session->context->cancelled()) {
      return false;
    }
    stop_session(session, options, callback);
    return true;
  };

  if (stop_if_cancelled()) {
    return;
  }

  // 前端只拿得到图片 ID，这里先换成实体 ID 再走实体库的存储定位。
  const auto entity_id = images->find_image_entity_id(session->image_id);
  if (stop_if_cancelled()) {
    return;
  }
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
    if (stop_if_cancelled()) {
      return;
    }
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
    if (stop_if_cancelled()) {
      return;
    }
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

  if (stop_if_cancelled()) {
    return;
  }

  const bool cache_enabled = image_cache && image_cache->enabled();

  // 变体缓存优先；命中后不再查源文件或访问存储。
  if (cache_enabled && !original) {
    auto found = image_cache->find(
        make_variant_cache_key(session->image_id, options.preset, processing));
    if (!found) {
      log_cache_error("variant find failed", found.error());
    } else if (*found) {
      auto object = std::make_shared<StoredImageCacheObject>(
          std::move(**found));
      session->cache_pin = object;
      if (stop_if_cancelled()) {
        return;
      }
      const std::string content_type =
          object->content_type().empty() ? "image/avif"
                                         : object->content_type();
      auto self = shared_from_this();
      post([self, session, object, content_type, options = options, etag,
            callback = std::move(callback)]() mutable {
        self->respond_cache_hit(session, object, content_type, options, etag,
                                callback);
      });
      return;
    }
  }

  // 变体未命中时，优先从源缓存转码，避免访问存储。
  if (cache_enabled) {
    auto found =
        image_cache->find(make_source_cache_key(session->image_id));
    if (!found) {
      log_cache_error("source find failed", found.error());
    } else if (*found) {
      auto object = std::make_shared<StoredImageCacheObject>(
          std::move(**found));
      session->cache_pin = object;
      if (stop_if_cancelled()) {
        return;
      }
      if (original) {
        const std::string content_type =
            object->content_type().empty() ? "application/octet-stream"
                                           : object->content_type();
        auto self = shared_from_this();
        post([self, session, object, content_type, options = options, etag,
              callback = std::move(callback)]() mutable {
          self->respond_cache_hit(session, object, content_type, options, etag,
                                  callback);
        });
        return;
      }

      build_variant_cache(session, options);
      auto result = process_cached_source(session, object->file_path(),
                                          options, etag, deadline, callback);
      if (result != ProcessResult::Fallback) {
        if (result == ProcessResult::TimedOut) {
          stop_session(session, options, callback);
        }
        return;
      }
      // 缓存文件损坏或无法解码：删掉该条目，继续走上游。
      object->remove();
      session->cache_pin.reset();
      session->variant_cache.reset();
      if (stop_if_cancelled()) {
        return;
      }
    }
  }

  auto opened = HttpByteStream::open(url);
  if (stop_if_cancelled()) {
    return;
  }
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

  // 先登记到 session，取消回调才能中断 open()/wait_started() 期间的阻塞。
  auto source = std::shared_ptr<HttpByteStream>(std::move(*opened));
  {
    std::lock_guard lock(mutex);
    session->stream = source;
  }

  // 确认上游真的开始返回数据；HTTP 状态码与响应头到这里才可知。
  const auto started = source->wait_started();
  if (stop_if_cancelled()) {
    return;
  }
  if (!started) {
    if (started.error() == ByteStreamError::TooLarge) {
      fail(session, options, drogon::k413RequestEntityTooLarge,
           "IMAGE_TOO_LARGE", "image exceeds the size limit", false, callback);
      return;
    }
    if (source->http_status() == 404) {
      fail(session, options, drogon::k404NotFound, "STORAGE_NOT_FOUND",
           "entity content not found", false, callback);
      return;
    }
    fail(session, options, drogon::k503ServiceUnavailable,
         "STORAGE_UNAVAILABLE", "storage service is unavailable", true,
         callback);
    return;
  }

  const std::string mime_type = media_type(source->content_type());
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
  if (stop_if_cancelled()) {
    return;
  }

  if (cache_enabled) {
    build_variant_cache(session, options);
    open_source_cache(session, mime_type);
  }

  if (original) {
    auto self = shared_from_this();
    post([self, session, mime_type, options, etag,
          callback = std::move(callback)]() mutable {
      self->respond_ok(session, mime_type, options, std::move(etag), callback);
    });
    return;
  }

  auto result = process_streaming(session, url, options, etag, deadline,
                                  callback);
  if (result == ProcessResult::Fallback) {
    // 流式解码失败：先把剩余上游字节收完并提交源缓存，再从缓存文件重试，
    // 避免第二次下载。
    if (drain_source_cache(session)) {
      result = process_cached_source(
          session, session->cache_pin->file_path(), options, etag, deadline,
          callback);
    }
    if (result == ProcessResult::Fallback) {
      result = process_temp(session, url, options, etag, deadline, callback);
    }
  }
  if (result == ProcessResult::Retry) {
    if (stop_if_cancelled()) {
      return;
    }
    fail(session, options, drogon::k503ServiceUnavailable,
         "STORAGE_UNAVAILABLE", "storage service is unavailable", true,
         callback);
    return;
  }
  if (result == ProcessResult::TimedOut) {
    // 客户端关闭时继续把源文件收完；超时/关停会因字节流被取消而放弃。
    drain_source_cache(session);
    stop_session(session, options, callback);
  }
}

ImageStreamer::Impl::ProcessResult ImageStreamer::Impl::process_streaming(
    const std::shared_ptr<StreamSession> &session, const std::string &url,
    const ImageStreamOptions &options, const std::string &etag,
    Clock::time_point deadline,
    std::function<void(const drogon::HttpResponsePtr &)> callback) {
  if (session->context->cancelled()) {
    return ProcessResult::TimedOut;
  }
  if (Clock::now() >= deadline) {
    session->context->cancel(CancelReason::Timeout);
    return ProcessResult::TimedOut;
  }

  VipsSource *raw_source = VIPS_SOURCE(vips_source_custom_new());
  if (raw_source == nullptr) {
    return ProcessResult::Fallback;
  }
  auto *context = new SourceContext();
  context->stream = session_stream(session);
  context->request = session->context;
  context->cache = session->source_cache;
  if (context->stream == nullptr) {
    delete context;
    g_object_unref(raw_source);
    return ProcessResult::Retry;
  }
  g_signal_connect_data(raw_source, "read", G_CALLBACK(source_read), context,
                        destroy_source_context, G_CONNECT_DEFAULT);
  VipsSourcePtr source(raw_source, VipsSourceDeleter{});

  // 先读文件头嗅探真实格式；预读的字节由 source_read 回放给 libvips。
  std::array<std::byte, kSniffBytes> probe{};
  std::size_t probed = 0;
  while (probed < probe.size()) {
    auto read = context->stream->read(std::span<std::byte>(
        probe.data() + probed, probe.size() - probed));
    if (!read || *read == 0) {
      break;
    }
    probed += *read;
  }
  if (probed > 0) {
    context->prefix.assign(probe.begin(),
                           probe.begin() + static_cast<std::ptrdiff_t>(probed));
  }
  const std::string load_mime = sniff_image_mime(
      reinterpret_cast<const std::uint8_t *>(probe.data()), probed);
  if (auto cache = session->source_cache) {
    if (probed > 0) {
      cache->write(std::span<const std::byte>(probe.data(), probed));
    }
    const std::string upstream_mime =
        media_type(context->stream->content_type());
    cache->set_content_type(load_mime.empty() ? upstream_mime : load_mime);
  }

  // GIF / WebP 可能有多帧；按多页加载，动图直接复用这次下载。
  VipsImage *raw_image = nullptr;
  if (mime_supports_multipage_load(load_mime)) {
    raw_image = vips_image_new_from_source(
        source.get(), "", "access", VIPS_ACCESS_SEQUENTIAL, "n", -1, nullptr);
  } else {
    raw_image = vips_image_new_from_source(
        source.get(), "", "access", VIPS_ACCESS_SEQUENTIAL, nullptr);
  }
  if (raw_image == nullptr) {
    log_vips_error("sequential source probe failed");
    if (session->context->cancelled()) {
      return ProcessResult::TimedOut;
    }
    if (Clock::now() >= deadline) {
      session->context->cancel(CancelReason::Timeout);
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

  if (image_is_animated(image.get())) {
    const int pages = vips_image_get_n_pages(image.get());
    const int page_height = vips_image_get_page_height(image.get());
    const auto total_pixels = static_cast<std::uint64_t>(width) *
                              static_cast<std::uint64_t>(page_height) *
                              static_cast<std::uint64_t>(pages);
    if (pages <= 0 || page_height <= 0 ||
        static_cast<std::uint64_t>(pages) > processing.animated_max_frames ||
        total_pixels > processing.animated_max_total_pixels) {
      fail(session, options, drogon::k413RequestEntityTooLarge,
           "IMAGE_TOO_LARGE", "animated image exceeds the size limit", false,
           callback);
      return ProcessResult::Done;
    }

    {
      std::lock_guard lock(mutex);
      session->image = image;
      session->source = source;
    }

    if (!acquire_encode_slot(session, deadline)) {
      stop_session(session, options, callback);
      return ProcessResult::Done;
    }
    if (session->context->cancelled()) {
      stop_session(session, options, callback);
      return ProcessResult::Done;
    }

    auto self = shared_from_this();
    post([self, session, image, source, options = options, etag = etag,
          deadline, callback = std::move(callback)]() mutable {
      self->respond_avif_stream(session, image, source, options, etag, deadline,
                                true, callback);
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
    stop_session(session, options, callback);
    return ProcessResult::Done;
  }

  if (session->context->cancelled()) {
    stop_session(session, options, callback);
    return ProcessResult::Done;
  }

  auto self = shared_from_this();
  post([self, session, image, source, options = options, etag = etag, deadline,
        callback = std::move(callback)]() mutable {
    self->respond_avif_stream(session, image, source, options, etag, deadline,
                              false, callback);
  });
  return ProcessResult::Done;
}

ImageStreamer::Impl::ProcessResult ImageStreamer::Impl::process_temp(
    const std::shared_ptr<StreamSession> &session, const std::string &url,
    const ImageStreamOptions &options, const std::string &etag,
    Clock::time_point deadline,
    std::function<void(const drogon::HttpResponsePtr &)> callback) {
  if (session->context->cancelled()) {
    return ProcessResult::TimedOut;
  }
  if (Clock::now() >= deadline) {
    session->context->cancel(CancelReason::Timeout);
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
  // 提前登记，取消回调可以中断 wait_started() 的阻塞。
  auto source = std::shared_ptr<HttpByteStream>(std::move(*opened));
  {
    std::lock_guard lock(mutex);
    session->stream = source;
  }
  if (!source->wait_started()) {
    return ProcessResult::Retry;
  }
  if (session->context->cancelled()) {
    return ProcessResult::TimedOut;
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
    if (session->context->cancelled()) {
      {
        std::lock_guard lock(mutex);
        session->stream.reset();
      }
      source->cancel();
      return ProcessResult::TimedOut;
    }
    if (Clock::now() >= deadline) {
      // 先取消再清理：取消回调会拿队列锁，不能持锁调用。
      session->context->cancel(CancelReason::Timeout);
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
  session->temp_pin = temp;

  std::string load_mime;
  {
    std::ifstream probe(temp->path(), std::ios::binary);
    if (probe) {
      std::array<char, kSniffBytes> head{};
      probe.read(head.data(), static_cast<std::streamsize>(head.size()));
      const auto got = static_cast<std::size_t>(probe.gcount());
      load_mime = sniff_image_mime(
          reinterpret_cast<const std::uint8_t *>(head.data()), got);
    }
  }

  VipsImage *raw_image = nullptr;
  if (mime_supports_multipage_load(load_mime)) {
    raw_image = vips_image_new_from_file(temp->path().c_str(), "access",
                                         VIPS_ACCESS_SEQUENTIAL, "n", -1,
                                         nullptr);
  } else {
    raw_image = vips_image_new_from_file(
        temp->path().c_str(), "access", VIPS_ACCESS_SEQUENTIAL, nullptr);
  }
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

  if (session->context->cancelled()) {
    stop_session(session, options, callback);
    return ProcessResult::Done;
  }

  if (image_is_animated(image.get())) {
    const int pages = vips_image_get_n_pages(image.get());
    const int page_height = vips_image_get_page_height(image.get());
    const auto total_pixels = static_cast<std::uint64_t>(width) *
                              static_cast<std::uint64_t>(page_height) *
                              static_cast<std::uint64_t>(pages);
    if (pages <= 0 || page_height <= 0 ||
        static_cast<std::uint64_t>(pages) > processing.animated_max_frames ||
        total_pixels > processing.animated_max_total_pixels) {
      fail(session, options, drogon::k413RequestEntityTooLarge,
           "IMAGE_TOO_LARGE", "animated image exceeds the size limit", false,
           callback);
      return ProcessResult::Done;
    }

    {
      std::lock_guard lock(mutex);
      session->image = image;
      session->source.reset();
    }

    if (!acquire_encode_slot(session, deadline)) {
      stop_session(session, options, callback);
      return ProcessResult::Done;
    }
    if (session->context->cancelled()) {
      stop_session(session, options, callback);
      return ProcessResult::Done;
    }

    auto self = shared_from_this();
    post([self, session, image, options = options, etag = etag, deadline,
          callback = std::move(callback)]() mutable {
      self->respond_avif_stream(session, image, VipsSourcePtr(), options, etag,
                                deadline, true, callback);
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
    stop_session(session, options, callback);
    return ProcessResult::Done;
  }

  if (session->context->cancelled()) {
    stop_session(session, options, callback);
    return ProcessResult::Done;
  }

  auto self = shared_from_this();
  post([self, session, image, options = options, etag = etag, deadline,
        callback = std::move(callback)]() mutable {
    self->respond_avif_stream(session, image, VipsSourcePtr(), options, etag,
                              deadline, false, callback);
  });
  return ProcessResult::Done;
}

ImageStreamer::Impl::ProcessResult ImageStreamer::Impl::process_cached_source(
    const std::shared_ptr<StreamSession> &session,
    const std::filesystem::path &path, const ImageStreamOptions &options,
    const std::string &etag, Clock::time_point deadline,
    std::function<void(const drogon::HttpResponsePtr &)> callback) {
  if (session->context->cancelled()) {
    return ProcessResult::TimedOut;
  }
  if (Clock::now() >= deadline) {
    session->context->cancel(CancelReason::Timeout);
    return ProcessResult::TimedOut;
  }

  reset_session_image(session);
  {
    std::lock_guard lock(mutex);
    session->stream.reset();
  }

  std::string load_mime;
  {
    std::ifstream probe(path, std::ios::binary);
    if (probe) {
      std::array<char, kSniffBytes> head{};
      probe.read(head.data(), static_cast<std::streamsize>(head.size()));
      const auto got = static_cast<std::size_t>(probe.gcount());
      load_mime = sniff_image_mime(
          reinterpret_cast<const std::uint8_t *>(head.data()), got);
    }
  }

  VipsImage *raw_image = nullptr;
  if (mime_supports_multipage_load(load_mime)) {
    raw_image = vips_image_new_from_file(path.c_str(), "access",
                                         VIPS_ACCESS_SEQUENTIAL, "n", -1,
                                         nullptr);
  } else {
    raw_image = vips_image_new_from_file(
        path.c_str(), "access", VIPS_ACCESS_SEQUENTIAL, nullptr);
  }
  if (raw_image == nullptr) {
    log_vips_error("cached source decode failed");
    return ProcessResult::Fallback;
  }
  VipsImagePtr image(raw_image, VipsImageDeleter{});

  const int width = vips_image_get_width(image.get());
  const int height = vips_image_get_height(image.get());
  if (width <= 0 || height <= 0) {
    return ProcessResult::Fallback;
  }

  if (session->context->cancelled()) {
    stop_session(session, options, callback);
    return ProcessResult::Done;
  }

  if (image_is_animated(image.get())) {
    const int pages = vips_image_get_n_pages(image.get());
    const int page_height = vips_image_get_page_height(image.get());
    const auto total_pixels = static_cast<std::uint64_t>(width) *
                              static_cast<std::uint64_t>(page_height) *
                              static_cast<std::uint64_t>(pages);
    if (pages <= 0 || page_height <= 0 ||
        static_cast<std::uint64_t>(pages) > processing.animated_max_frames ||
        total_pixels > processing.animated_max_total_pixels) {
      fail(session, options, drogon::k413RequestEntityTooLarge,
           "IMAGE_TOO_LARGE", "animated image exceeds the size limit", false,
           callback);
      return ProcessResult::Done;
    }

    {
      std::lock_guard lock(mutex);
      session->image = image;
      session->source.reset();
    }

    if (!acquire_encode_slot(session, deadline)) {
      stop_session(session, options, callback);
      return ProcessResult::Done;
    }
    if (session->context->cancelled()) {
      stop_session(session, options, callback);
      return ProcessResult::Done;
    }

    auto self = shared_from_this();
    post([self, session, image, options = options, etag = etag, deadline,
          callback = std::move(callback)]() mutable {
      self->respond_avif_stream(session, image, VipsSourcePtr(), options, etag,
                                deadline, true, callback);
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
    stop_session(session, options, callback);
    return ProcessResult::Done;
  }

  if (session->context->cancelled()) {
    stop_session(session, options, callback);
    return ProcessResult::Done;
  }

  auto self = shared_from_this();
  post([self, session, image, options = options, etag = etag, deadline,
        callback = std::move(callback)]() mutable {
    self->respond_avif_stream(session, image, VipsSourcePtr(), options, etag,
                              deadline, false, callback);
  });
  return ProcessResult::Done;
}

void ImageStreamer::Impl::respond_ok(
    const std::shared_ptr<StreamSession> &session,
    const std::string &mime_type, const ImageStreamOptions &options,
    std::string etag,
    const std::function<void(const drogon::HttpResponsePtr &)> &callback) {
  if (session->context->cancelled()) {
    stop_session(session, options, callback);
    return;
  }
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
    session->context->cancel(CancelReason::ClientClosed);
    self->finish(session);
  });

  callback(response);
}

void ImageStreamer::Impl::respond_cache_hit(
    const std::shared_ptr<StreamSession> &session,
    std::shared_ptr<StoredImageCacheObject> object,
    const std::string &content_type, ImageStreamOptions options,
    std::string etag,
    const std::function<void(const drogon::HttpResponsePtr &)> &callback) {
  if (session->context->cancelled()) {
    stop_session(session, options, callback);
    return;
  }
  auto self = shared_from_this();
  auto response = drogon::HttpResponse::newAsyncStreamResponse(
      [self, session, object](drogon::ResponseStreamPtr stream) {
        self->pump_cache_file(session, object, std::move(stream));
      });
  response->setStatusCode(drogon::k200OK);
  response->setContentTypeString(content_type);
  response->addHeader(
      "Content-Disposition",
      content_disposition(session->image_id, content_type, options.download));
  response->addHeader("ETag", etag);
  response->addHeader("Cache-Control", "private, max-age=3600");
  response->addHeader("X-Content-Type-Options", "nosniff");

  drogon::app().getLoop()->runAfter(kStartTimeoutSeconds, [self, session] {
    bool expected = false;
    if (!session->started.compare_exchange_strong(expected, true)) {
      return;
    }
    session->context->cancel(CancelReason::ClientClosed);
    self->finish(session);
  });

  callback(response);
}

void ImageStreamer::Impl::respond_avif_stream(
    const std::shared_ptr<StreamSession> &session, VipsImagePtr image,
    VipsSourcePtr source, ImageStreamOptions options, std::string etag,
    Clock::time_point deadline, bool animated,
    const std::function<void(const drogon::HttpResponsePtr &)> &callback) {
  if (session->context->cancelled()) {
    stop_session(session, options, callback);
    return;
  }
  auto self = shared_from_this();
  auto response = drogon::HttpResponse::newAsyncStreamResponse(
      [self, session, image, source, options, deadline, animated](
          drogon::ResponseStreamPtr stream) mutable {
        self->pump_avif(session, image, source, options, deadline, animated,
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
    session->context->cancel(CancelReason::ClientClosed);
    self->finish(session);
  });

  callback(response);
}

void ImageStreamer::Impl::respond_redirect(
    const std::shared_ptr<StreamSession> &session, std::string url,
    const std::function<void(const drogon::HttpResponsePtr &)> &callback) {
  if (session->context->cancelled()) {
    finish(session);
    return;
  }
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

void ImageStreamer::Impl::stop_session(
    const std::shared_ptr<StreamSession> &session,
    const ImageStreamOptions &options,
    const std::function<void(const drogon::HttpResponsePtr &)> &callback) {
  if (session->context->reason() == CancelReason::Timeout) {
    fail(session, options, drogon::k504GatewayTimeout,
         "IMAGE_PROCESSING_TIMEOUT", "image processing timed out", false,
         callback);
    return;
  }
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
  auto cache = session->source_cache;

  auto self = shared_from_this();
  std::thread([self, session, source = std::move(source),
               cache = std::move(cache), stream = std::move(stream)]() mutable {
    std::vector<std::byte> buffer(kChunkBytes);
    bool send_failed = false;
    while (source != nullptr) {
      // 客户端断开但源缓存仍在写入时，继续读完字节流；其余取消原因直接退出。
      if (session->context->cancelled() && !cache) {
        break;
      }
      if (session->context->cancelled()) {
        send_failed = true;
      }
      const auto read = source->read(buffer);
      if (!read) {
        break;
      }
      if (*read == 0) {
        if (cache) {
          cache->mark_complete();
        }
        break;
      }
      if (cache) {
        cache->write(std::span<const std::byte>(buffer.data(), *read));
      }
      if (!send_failed) {
        const std::string chunk(reinterpret_cast<const char *>(buffer.data()),
                                *read);
        if (!stream->send(chunk)) {
          // 客户端已断开；源缓存还在写时继续把剩余字节收完。
          session->context->cancel(CancelReason::ClientClosed);
          send_failed = true;
          if (!cache) {
            break;
          }
        }
      }
    }
    if (cache) {
      const bool stored = cache->finish();
      session->source_cache_active.store(false, std::memory_order_relaxed);
      if (stored) {
        session->cache_pin = cache->object();
      } else {
        session->cache_pin.reset();
      }
    }
    stream->close();
    // 先释放响应流，再析构字节流（析构会中断 curl 并回收线程）。
    stream.reset();
    source.reset();
    self->finish(session);
  }).detach();
}

void ImageStreamer::Impl::pump_cache_file(
    const std::shared_ptr<StreamSession> &session,
    std::shared_ptr<StoredImageCacheObject> object,
    drogon::ResponseStreamPtr stream) {
  session->started.store(true, std::memory_order_relaxed);

  auto self = shared_from_this();
  std::thread([self, session, object = std::move(object),
               stream = std::move(stream)]() mutable {
    std::vector<char> buffer(kChunkBytes);
    while (!session->context->cancelled()) {
      auto &input = object->read();
      input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      const auto got = input.gcount();
      if (got <= 0) {
        break;
      }
      if (!stream->send(std::string(buffer.data(),
                                   static_cast<std::size_t>(got)))) {
        session->context->cancel(CancelReason::ClientClosed);
        break;
      }
    }
    stream->close();
    stream.reset();
    object.reset();
    self->finish(session);
  }).detach();
}

void ImageStreamer::Impl::pump_avif(
    const std::shared_ptr<StreamSession> &session, VipsImagePtr image,
    VipsSourcePtr source, ImageStreamOptions options, Clock::time_point deadline,
    bool animated, drogon::ResponseStreamPtr stream) {
  session->started.store(true, std::memory_order_relaxed);

  const std::string suffix = avif_save_options(processing);
  auto self = shared_from_this();
  if (session->context->cancelled()) {
    stream->close();
    source.reset();
    self->drain_source_cache(session);
    self->finish(session);
    return;
  }
  std::thread([self, session, image = std::move(image),
               source = std::move(source), options = std::move(options),
               deadline, animated, suffix,
               stream = std::move(stream)]() mutable {
    const auto now = Clock::now();
    if (now >= deadline) {
      session->context->cancel(CancelReason::Timeout);
      stream->close();
      source.reset();
      self->drain_source_cache(session);
      self->finish(session);
      return;
    }

    TimeoutGuard guard(deadline - now, [session] {
      session->context->cancel(CancelReason::Timeout);
    });

    // 关闭响应并提交源缓存；variant_commit 为 true 时再提交变体缓存。
    const auto finalize = [&](bool variant_commit) {
      stream->close();
      stream.reset();
      // 先销毁 vips source，再读取/提交源缓存写对象。
      source.reset();
      self->drain_source_cache(session);
      if (variant_commit) {
        auto variant = session->variant_cache;
        if (variant) {
          const auto committed = variant->commit();
          if (!committed) {
            log_cache_error("variant commit failed", committed.error());
          } else if (*committed != ImageCacheCommit::Committed) {
            std::cerr
                << "image cache: variant skipped because it exceeds the limit\n";
          }
        }
      }
      session->variant_cache.reset();
      self->finish(session);
    };

    // 动图走 libheif 序列编码：保留每帧时长和循环次数，逐块写出。
    if (animated) {
      AnimatedAvifConfig config;
      config.quality = self->processing.animated_quality;
      config.speed = self->processing.animated_speed;
      const int target = preset_dimension(options.preset);
      config.max_dimension =
          target > 0 ? static_cast<std::uint32_t>(target) : 0;
      config.max_frames = self->processing.animated_max_frames;
      config.max_total_pixels = self->processing.animated_max_total_pixels;
      config.max_output_bytes = self->processing.animated_max_output_bytes;

      auto *stream_ptr = stream.get();
      auto variant = session->variant_cache;
      auto cache_failed = std::make_shared<std::atomic<bool>>(false);
      const auto result = encode_animated_avif(
          image.get(), config,
          [session] { return session->context->cancelled(); },
          [stream_ptr, variant, cache_failed](const void *data,
                                              std::size_t size) {
            if (variant && !cache_failed->load(std::memory_order_relaxed)) {
              auto &out = variant->write();
              out.write(static_cast<const char *>(data),
                        static_cast<std::streamsize>(size));
              if (!out.good()) {
                cache_failed->store(true, std::memory_order_relaxed);
              }
            }
            return stream_ptr->send(
                std::string(static_cast<const char *>(data), size));
          });
      if (result != AnimatedAvifError::None) {
        std::cerr << "animated avif encoding failed: " << to_string(result)
                  << std::endl;
        if (result == AnimatedAvifError::SinkFailed) {
          session->context->cancel(CancelReason::ClientClosed);
        }
      }
      finalize(result == AnimatedAvifError::None &&
               !cache_failed->load(std::memory_order_relaxed));
      return;
    }

    const int dimension = preset_dimension(options.preset);
    VipsImage *thumbnail_raw = nullptr;
    if (dimension <= 0 ||
        vips_thumbnail_image(image.get(), &thumbnail_raw, dimension, "height",
                             dimension, "size", VIPS_SIZE_DOWN, "auto_rotate",
                             TRUE, nullptr) != 0) {
      log_vips_error("thumbnail failed");
      finalize(false);
      return;
    }
    VipsImagePtr thumbnail(thumbnail_raw, VipsImageDeleter{});

    VipsTarget *target = VIPS_TARGET(vips_target_custom_new());
    if (target == nullptr) {
      log_vips_error("target creation failed");
      finalize(false);
      return;
    }
    auto *context = new TargetContext();
    context->stream = stream.get();
    context->request = session->context.get();
    context->cache = session->variant_cache;
    g_signal_connect_data(target, "write", G_CALLBACK(target_write), context,
                          destroy_target_context, G_CONNECT_DEFAULT);

    const int result = vips_image_write_to_target(
        thumbnail.get(), suffix.c_str(), target, nullptr);
    const bool send_failed = context->failed;
    const bool cache_failed = context->cache_failed;
    if (result != 0) {
      log_vips_error("avif encoding failed");
    }
    g_object_unref(target);
    if (send_failed) {
      session->context->cancel(CancelReason::ClientClosed);
    }
    finalize(result == 0 && !send_failed && !cache_failed);
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
  if (registry && session->context) {
    registry->unregister_context(session->context);
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
    // 取消回调负责中断阻塞中的 read()/wait_started() 并打断 vips 编码。
    session->context->cancel(CancelReason::Shutdown);
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
                             std::filesystem::path work_dir,
                             std::shared_ptr<ConnectionRegistry> registry,
                             std::shared_ptr<ImageCacheStore> image_cache)
    : impl_(std::make_shared<Impl>(std::move(images), std::move(storage), mode,
                                   processing, std::move(work_dir),
                                   std::move(registry),
                                   std::move(image_cache))) {
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
