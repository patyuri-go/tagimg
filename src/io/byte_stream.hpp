#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <atomic>

#include "common/result.hpp"

namespace tagimg {

enum class ByteStreamError {
  InvalidUrl,
  ConnectFailed,
  Timeout,
  HttpError,
  TransferFailed,
  TooLarge,
  ReadFailed,
};

[[nodiscard]] std::string_view to_string(ByteStreamError error) noexcept;

// 顺序读取字节的抽象。read() 返回 0 表示流正常结束，返回错误表示
// 连接中断、HTTP 失败或超过大小限制。
class ByteStream {
public:
  virtual ~ByteStream() = default;

  ByteStream(const ByteStream &) = delete;
  ByteStream &operator=(const ByteStream &) = delete;

  [[nodiscard]] virtual Result<std::size_t, ByteStreamError>
  read(std::span<std::byte> buffer) = 0;

protected:
  ByteStream() = default;
};

struct ByteStreamOptions {
  // 允许读取的最大字节数，超过后流会中断并返回 TooLarge。
  std::size_t max_bytes = 50u * 1024u * 1024u;
  std::chrono::seconds connect_timeout{10};
  // 连续多少秒收不到数据就中断传输。
  std::chrono::seconds idle_timeout{30};
};

// 上游 HTTP 内容流的字节流。
//
// 内部用一个后台线程执行 libcurl 传输，写回调把数据放进有界队列，
// read() 从队列取数据。因此内存占用只与队列长度有关，不会把整个
// 文件读进内存。对象析构时中断传输并回收线程。
class HttpByteStream final : public ByteStream {
public:
  static Result<std::unique_ptr<HttpByteStream>, ByteStreamError>
  open(std::string url, ByteStreamOptions options = {});

  ~HttpByteStream() override;

  HttpByteStream(const HttpByteStream &) = delete;
  HttpByteStream &operator=(const HttpByteStream &) = delete;
  HttpByteStream(HttpByteStream &&) = delete;
  HttpByteStream &operator=(HttpByteStream &&) = delete;

  [[nodiscard]] Result<std::size_t, ByteStreamError>
  read(std::span<std::byte> buffer) override;

  // 等到首批数据到达或传输结束，用于在发出响应前确认节点确实可用。
  //
  // open() 只是启动后台传输，HTTP 状态码与响应头要等传输开始才知道，
  // 所以需要这一步。已有数据排队时视为成功，数据不会被消耗。
  [[nodiscard]] Result<void, ByteStreamError> wait_started();

  // 中断传输并唤醒阻塞中的 read()/wait_started()。
  //
  // 用于服务关闭时让工作线程尽快退出；之后 read() 会返回 0 或错误。
  void cancel() noexcept;

  // 已经从网络收到的总字节数，包含还没被 read() 取走的部分。
  [[nodiscard]] std::uint64_t bytes_received() const noexcept;

  // 上游最终响应的 Content-Type / Content-Length；缺失时分别为空/nullopt。
  [[nodiscard]] std::string content_type() const;
  [[nodiscard]] std::optional<std::uint64_t> content_length() const;

  // 上游最终 HTTP 状态码；wait_started() 失败时也能取到。
  [[nodiscard]] long http_status() const;

private:
  // 队列里最多保留的块数，每块通常不超过 16 KB。
  static constexpr std::size_t kMaxQueuedChunks = 8;

  HttpByteStream(std::string url, ByteStreamOptions options, void *handle);

  static std::size_t write_callback(char *data, std::size_t size,
                                    std::size_t count, void *user_data);

  static std::size_t header_callback(char *data, std::size_t size,
                                     std::size_t count, void *user_data);

  // curl 的进度回调，返回非 0 会中断传输；析构时用它打断还在跑的下载。
  static int progress_callback(void *user_data, std::int64_t download_total,
                               std::int64_t downloaded,
                               std::int64_t upload_total,
                               std::int64_t uploaded);

  // libcurl 写回调的入口，返回实际接收的字节数，返回 0 表示中止传输。
  std::size_t on_data(const char *data, std::size_t size);

  // 解析一行响应头；返回 false 会中止传输。
  bool on_header(std::string_view line);

  // 后台线程：执行 curl_easy_perform，结束后标记流状态。
  void run();

  std::string url_;
  ByteStreamOptions options_;
  void *handle_ = nullptr;
  std::thread worker_;
  std::atomic<bool> stop_{false};

  mutable std::mutex mutex_;
  std::condition_variable data_ready_;
  std::condition_variable space_ready_;
  std::deque<std::vector<std::byte>> chunks_;
  std::size_t front_offset_ = 0;
  std::uint64_t received_ = 0;
  bool finished_ = false;
  bool aborted_ = false;
  std::optional<ByteStreamError> error_;

  // 响应头与状态码由 header 回调写入，单独加锁避免和队列等待互相阻塞。
  mutable std::mutex header_mutex_;
  std::string content_type_;
  std::optional<std::uint64_t> content_length_;
  long http_status_ = 0;
};

} // namespace tagimg
