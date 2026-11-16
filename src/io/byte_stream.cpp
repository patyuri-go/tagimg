#include "io/byte_stream.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <utility>

#include <curl/curl.h>

namespace tagimg {
namespace {

std::once_flag g_curl_init_once;
CURLcode g_curl_init_result = CURLE_FAILED_INIT;

void ensure_curl_initialized() {
  std::call_once(g_curl_init_once, [] {
    g_curl_init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
  });
}

bool is_http_url(std::string_view url) {
  const bool has_scheme =
      url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
  if (!has_scheme) {
    return false;
  }
  const std::size_t scheme_end = url.find("://") + 3;
  return url.size() > scheme_end && url[scheme_end] != '/';
}

ByteStreamError map_curl_error(CURLcode code) {
  switch (code) {
  case CURLE_COULDNT_CONNECT:
  case CURLE_COULDNT_RESOLVE_HOST:
  case CURLE_COULDNT_RESOLVE_PROXY:
  case CURLE_SSL_CONNECT_ERROR:
    return ByteStreamError::ConnectFailed;
  case CURLE_OPERATION_TIMEDOUT:
    return ByteStreamError::Timeout;
  case CURLE_HTTP_RETURNED_ERROR:
    return ByteStreamError::HttpError;
  default:
    return ByteStreamError::TransferFailed;
  }
}

// 直链解析最多跟随的重定向次数。
constexpr long kResolveMaxRedirects = 20;
constexpr long kResolveConnectTimeoutSeconds = 10;
constexpr long kResolveTimeoutSeconds = 20;

struct RedirectProbe {
  CURLcode code = CURLE_FAILED_INIT;
  long status = 0;
  std::string final_url;
};

// GET 探测时只要收到首块数据就返回 0 中止，避免下载完整文件。
std::size_t discard_body(char * /*data*/, std::size_t /*size*/,
                         std::size_t /*count*/, void * /*user_data*/) {
  return 0;
}

RedirectProbe probe_redirects(const std::string &url) {
  RedirectProbe result;
  CURL *handle = curl_easy_init();
  if (handle == nullptr) {
    return result;
  }

  const auto set = [handle](CURLoption option, auto value) {
    return curl_easy_setopt(handle, option, value) == CURLE_OK;
  };
  const bool configured =
      set(CURLOPT_URL, url.c_str()) && set(CURLOPT_FOLLOWLOCATION, 1L) &&
      set(CURLOPT_MAXREDIRS, kResolveMaxRedirects) &&
      set(CURLOPT_CONNECTTIMEOUT, kResolveConnectTimeoutSeconds) &&
      set(CURLOPT_TIMEOUT, kResolveTimeoutSeconds) &&
      set(CURLOPT_NOSIGNAL, 1L) && set(CURLOPT_ACCEPT_ENCODING, "") &&
      set(CURLOPT_USERAGENT, "tagimg/0.1") &&
      // 只允许 HTTP(S)，避免上游把探测重定向到本地协议。
      set(CURLOPT_PROTOCOLS_STR, "http,https") &&
      set(CURLOPT_REDIR_PROTOCOLS_STR, "http,https") &&
      // Range 只取一个字节；服务器忽略 Range 时由写回调立即中止。
      set(CURLOPT_RANGE, "0-0") && set(CURLOPT_WRITEFUNCTION, discard_body);
  if (!configured) {
    curl_easy_cleanup(handle);
    return result;
  }

  result.code = curl_easy_perform(handle);
  curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &result.status);
  char *effective = nullptr;
  if (curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &effective) ==
          CURLE_OK &&
      effective != nullptr) {
    result.final_url = effective;
  }
  curl_easy_cleanup(handle);
  return result;
}

bool redirect_probe_ok(const RedirectProbe &probe) {
  // GET 探测主动中止会得到
  // CURLE_WRITE_ERROR，只要响应头和直链已经拿到就算成功。
  const bool transport_ok =
      probe.code == CURLE_OK || probe.code == CURLE_WRITE_ERROR;
  return transport_ok && probe.status >= 200 && probe.status < 300 &&
         is_http_url(probe.final_url);
}

} // namespace

std::string_view to_string(ByteStreamError error) noexcept {
  switch (error) {
  case ByteStreamError::InvalidUrl:
    return "InvalidUrl";
  case ByteStreamError::ConnectFailed:
    return "ConnectFailed";
  case ByteStreamError::Timeout:
    return "Timeout";
  case ByteStreamError::HttpError:
    return "HttpError";
  case ByteStreamError::TransferFailed:
    return "TransferFailed";
  case ByteStreamError::TooLarge:
    return "TooLarge";
  case ByteStreamError::ReadFailed:
    return "ReadFailed";
  }
  return "Unknown";
}

std::string_view to_string(RedirectResolveError error) noexcept {
  switch (error) {
  case RedirectResolveError::InvalidUrl:
    return "InvalidUrl";
  case RedirectResolveError::RequestFailed:
    return "RequestFailed";
  case RedirectResolveError::HttpError:
    return "HttpError";
  case RedirectResolveError::TooManyRedirects:
    return "TooManyRedirects";
  }
  return "Unknown";
}

Result<std::string, RedirectResolveError>
resolve_redirect_url(const std::string &url) {
  ensure_curl_initialized();
  if (g_curl_init_result != CURLE_OK) {
    return RedirectResolveError::RequestFailed;
  }
  if (!is_http_url(url)) {
    return RedirectResolveError::InvalidUrl;
  }

  const RedirectProbe get = probe_redirects(url);
  if (redirect_probe_ok(get)) {
    return get.final_url;
  }

  if (get.code == CURLE_TOO_MANY_REDIRECTS) {
    return RedirectResolveError::TooManyRedirects;
  }
  if (get.status >= 400) {
    return RedirectResolveError::HttpError;
  }
  return RedirectResolveError::RequestFailed;
}

Result<std::unique_ptr<WebdavByteStream>, ByteStreamError>
WebdavByteStream::open(std::string url, ByteStreamOptions options) {
  ensure_curl_initialized();
  if (g_curl_init_result != CURLE_OK) {
    return ByteStreamError::TransferFailed;
  }
  if (!is_http_url(url)) {
    return ByteStreamError::InvalidUrl;
  }

  CURL *handle = curl_easy_init();
  if (handle == nullptr) {
    return ByteStreamError::TransferFailed;
  }

  // URL 由 libcurl 复制保存；这些选项在对象构造前就可以设置。
  const auto set = [handle](CURLoption option, auto value) {
    return curl_easy_setopt(handle, option, value) == CURLE_OK;
  };
  const bool configured =
      set(CURLOPT_URL, url.c_str()) && set(CURLOPT_FOLLOWLOCATION, 1L) &&
      set(CURLOPT_MAXREDIRS, 5L) &&
      set(CURLOPT_CONNECTTIMEOUT,
          static_cast<long>(options.connect_timeout.count())) &&
      set(CURLOPT_LOW_SPEED_LIMIT, 1L) &&
      set(CURLOPT_LOW_SPEED_TIME,
          static_cast<long>(options.idle_timeout.count())) &&
      set(CURLOPT_NOSIGNAL, 1L) && set(CURLOPT_ACCEPT_ENCODING, "") &&
      set(CURLOPT_USERAGENT, "tagimg/0.1");
  if (!configured) {
    curl_easy_cleanup(handle);
    return ByteStreamError::TransferFailed;
  }

  try {
    return std::unique_ptr<WebdavByteStream>(
        new WebdavByteStream(std::move(url), options, handle));
  } catch (const std::exception &) {
    curl_easy_cleanup(handle);
    return ByteStreamError::TransferFailed;
  }
}

WebdavByteStream::WebdavByteStream(std::string url, ByteStreamOptions options,
                                   void *handle)
    : url_(std::move(url)), options_(options), handle_(handle) {
  auto *curl = static_cast<CURL *>(handle_);
  if (curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                       &WebdavByteStream::write_callback) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, this) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                       &WebdavByteStream::progress_callback) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this) != CURLE_OK) {
    throw std::runtime_error("failed to configure curl callbacks");
  }
  worker_ = std::thread([this] { run(); });
}

WebdavByteStream::~WebdavByteStream() {
  stop_.store(true);
  {
    std::lock_guard lock(mutex_);
    aborted_ = true;
  }
  data_ready_.notify_all();
  space_ready_.notify_all();

  if (worker_.joinable()) {
    worker_.join();
  }
  if (handle_ != nullptr) {
    curl_easy_cleanup(static_cast<CURL *>(handle_));
  }
}

int WebdavByteStream::progress_callback(void *user_data,
                                        std::int64_t /*download_total*/,
                                        std::int64_t /*downloaded*/,
                                        std::int64_t /*upload_total*/,
                                        std::int64_t /*uploaded*/) {
  const auto *stream = static_cast<const WebdavByteStream *>(user_data);
  return stream->stop_.load() ? 1 : 0;
}

std::size_t WebdavByteStream::write_callback(char *data, std::size_t size,
                                             std::size_t count,
                                             void *user_data) {
  auto *stream = static_cast<WebdavByteStream *>(user_data);
  return stream->on_data(data, size * count);
}

std::size_t WebdavByteStream::on_data(const char *data, std::size_t size) {
  std::unique_lock lock(mutex_);
  if (aborted_) {
    return 0;
  }
  if (received_ + size > options_.max_bytes) {
    if (!error_) {
      error_ = ByteStreamError::TooLarge;
    }
    aborted_ = true;
    return 0;
  }

  // 队列满时让后台传输等待，避免把整个文件堆在内存里。
  space_ready_.wait(
      lock, [this] { return aborted_ || chunks_.size() < kMaxQueuedChunks; });
  if (aborted_) {
    return 0;
  }

  const auto *begin = reinterpret_cast<const std::byte *>(data);
  chunks_.emplace_back(begin, begin + size);
  received_ += size;
  data_ready_.notify_one();
  return size;
}

Result<std::size_t, ByteStreamError>
WebdavByteStream::read(std::span<std::byte> buffer) {
  if (buffer.empty()) {
    return std::size_t{0};
  }

  std::unique_lock lock(mutex_);
  data_ready_.wait(
      lock, [this] { return !chunks_.empty() || finished_ || aborted_; });

  std::size_t copied = 0;
  while (copied < buffer.size() && !chunks_.empty()) {
    std::vector<std::byte> &chunk = chunks_.front();
    const std::size_t available = chunk.size() - front_offset_;
    const std::size_t count = std::min(available, buffer.size() - copied);
    std::memcpy(buffer.data() + copied, chunk.data() + front_offset_, count);

    copied += count;
    front_offset_ += count;
    if (front_offset_ == chunk.size()) {
      chunks_.pop_front();
      front_offset_ = 0;
    }
  }

  if (copied > 0) {
    space_ready_.notify_one();
    return copied;
  }
  if (error_) {
    return *error_;
  }
  return std::size_t{0};
}

std::uint64_t WebdavByteStream::bytes_received() const noexcept {
  std::lock_guard lock(mutex_);
  return received_;
}

Result<void, ByteStreamError> WebdavByteStream::wait_started() {
  std::unique_lock lock(mutex_);
  data_ready_.wait(
      lock, [this] { return !chunks_.empty() || finished_ || aborted_; });

  if (error_) {
    return *error_;
  }
  if (chunks_.empty()) {
    // 传输已经结束却一个字节都没收到：对图片来说等同节点不可用。
    return ByteStreamError::HttpError;
  }
  return Result<void, ByteStreamError>::ok();
}

void WebdavByteStream::cancel() noexcept {
  stop_.store(true);
  {
    std::lock_guard lock(mutex_);
    aborted_ = true;
  }
  data_ready_.notify_all();
  space_ready_.notify_all();
}

void WebdavByteStream::run() {
  auto *curl = static_cast<CURL *>(handle_);
  const CURLcode code = curl_easy_perform(curl);

  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

  std::lock_guard lock(mutex_);
  if (!error_) {
    if (code != CURLE_OK) {
      error_ = map_curl_error(code);
    } else if (status < 200 || status >= 300) {
      error_ = ByteStreamError::HttpError;
    }
  }
  finished_ = true;
  data_ready_.notify_all();
}

} // namespace tagimg
