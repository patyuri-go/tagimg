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

Result<std::unique_ptr<HttpByteStream>, ByteStreamError>
HttpByteStream::open(std::string url, ByteStreamOptions options) {
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
    return std::unique_ptr<HttpByteStream>(
        new HttpByteStream(std::move(url), options, handle));
  } catch (const std::exception &) {
    curl_easy_cleanup(handle);
    return ByteStreamError::TransferFailed;
  }
}

HttpByteStream::HttpByteStream(std::string url, ByteStreamOptions options,
                               void *handle)
    : url_(std::move(url)), options_(options), handle_(handle) {
  auto *curl = static_cast<CURL *>(handle_);
  if (curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                       &HttpByteStream::write_callback) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, this) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
                       &HttpByteStream::header_callback) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_HEADERDATA, this) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                       &HttpByteStream::progress_callback) != CURLE_OK ||
      curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this) != CURLE_OK) {
    throw std::runtime_error("failed to configure curl callbacks");
  }
  worker_ = std::thread([this] { run(); });
}

HttpByteStream::~HttpByteStream() {
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

int HttpByteStream::progress_callback(void *user_data,
                                        std::int64_t /*download_total*/,
                                        std::int64_t /*downloaded*/,
                                        std::int64_t /*upload_total*/,
                                        std::int64_t /*uploaded*/) {
  const auto *stream = static_cast<const HttpByteStream *>(user_data);
  return stream->stop_.load() ? 1 : 0;
}

std::size_t HttpByteStream::write_callback(char *data, std::size_t size,
                                             std::size_t count,
                                             void *user_data) {
  auto *stream = static_cast<HttpByteStream *>(user_data);
  return stream->on_data(data, size * count);
}

std::size_t HttpByteStream::header_callback(char *data, std::size_t size,
                                            std::size_t count,
                                            void *user_data) {
  auto *stream = static_cast<HttpByteStream *>(user_data);
  return stream->on_header(
             std::string_view(data, size * count))
             ? size * count
             : 0;
}

bool HttpByteStream::on_header(std::string_view line) {
  if (line.rfind("HTTP/", 0) == 0) {
    // 新响应开始（含重定向的中转响应）：清掉上一条响应的元数据。
    long status = 0;
    const auto space = line.find(' ');
    if (space != std::string_view::npos) {
      for (std::size_t index = space + 1;
           index < line.size() && line[index] >= '0' && line[index] <= '9';
           ++index) {
        status = status * 10 + (line[index] - '0');
      }
    }
    std::lock_guard lock(header_mutex_);
    http_status_ = status;
    content_type_.clear();
    content_length_.reset();
    return true;
  }

  const auto colon = line.find(':');
  if (colon == std::string_view::npos) {
    return true;
  }
  std::string_view name = line.substr(0, colon);
  std::string_view value = line.substr(colon + 1);
  while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
    name.remove_suffix(1);
  }
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n' ||
                            value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }

  std::string lowered(name);
  for (char &character : lowered) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }

  std::lock_guard lock(header_mutex_);
  if (lowered == "content-type") {
    content_type_.assign(value);
  } else if (lowered == "content-length") {
    std::uint64_t length = 0;
    bool valid = !value.empty();
    for (const char character : value) {
      if (character < '0' || character > '9') {
        valid = false;
        break;
      }
      const auto digit = static_cast<std::uint64_t>(character - '0');
      if (length > (UINT64_MAX - digit) / 10) {
        valid = false;
        break;
      }
      length = length * 10 + digit;
    }
    if (valid) {
      content_length_ = length;
    }
  }
  return true;
}

std::string HttpByteStream::content_type() const {
  std::lock_guard lock(header_mutex_);
  return content_type_;
}

std::optional<std::uint64_t> HttpByteStream::content_length() const {
  std::lock_guard lock(header_mutex_);
  return content_length_;
}

long HttpByteStream::http_status() const {
  std::lock_guard lock(header_mutex_);
  return http_status_;
}

std::size_t HttpByteStream::on_data(const char *data, std::size_t size) {
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
HttpByteStream::read(std::span<std::byte> buffer) {
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

std::uint64_t HttpByteStream::bytes_received() const noexcept {
  std::lock_guard lock(mutex_);
  return received_;
}

Result<void, ByteStreamError> HttpByteStream::wait_started() {
  std::unique_lock lock(mutex_);
  data_ready_.wait(
      lock, [this] { return !chunks_.empty() || finished_ || aborted_; });

  if (error_) {
    return *error_;
  }
  // 有些错误响应也带 body，必须先按状态码拒绝。
  const long status = http_status();
  if (status != 0 && (status < 200 || status >= 300)) {
    return ByteStreamError::HttpError;
  }
  if (chunks_.empty()) {
    // 传输已经结束却一个字节都没收到：对图片来说等同节点不可用。
    return ByteStreamError::HttpError;
  }
  return Result<void, ByteStreamError>::ok();
}

void HttpByteStream::cancel() noexcept {
  stop_.store(true);
  {
    std::lock_guard lock(mutex_);
    aborted_ = true;
  }
  data_ready_.notify_all();
  space_ready_.notify_all();
}

void HttpByteStream::run() {
  auto *curl = static_cast<CURL *>(handle_);
  const CURLcode code = curl_easy_perform(curl);

  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  {
    std::lock_guard header_lock(header_mutex_);
    if (status != 0) {
      http_status_ = status;
    }
  }

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
