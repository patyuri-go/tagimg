#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <trantor/net/EventLoopThread.h>

#include "common/result.hpp"

namespace tagimg {

enum class TaggerError {
  SidecarUnavailable,
  Timeout,
  InvalidImage,
  ImageTooLarge,
  HttpError,
  InvalidResponse,
  FileReadError,
};

[[nodiscard]] std::string_view to_string(TaggerError error) noexcept;

struct TagScore {
  std::string name;
  float score;
};

struct TaggingResult {
  std::vector<TagScore> general;
  std::vector<TagScore> character;
  std::vector<TagScore> rating;
};

using TaggerResult = Result<TaggingResult, TaggerError>;

// 本地 Python tagger sidecar 的客户端封装。
//
// 本对象不嵌入 Python，而是把原始图片字节 POST 到 sidecar 的
// POST /v1/tag/image，再解析返回的 JSON。
class WdTagger {
public:
  static constexpr std::size_t kMaxImageBytes = 50 * 1024 * 1024;

  explicit WdTagger(
      std::string endpoint, float threshold,
      std::chrono::milliseconds timeout = std::chrono::seconds(30));

  WdTagger(const WdTagger &) = delete;
  WdTagger &operator=(const WdTagger &) = delete;
  WdTagger(WdTagger &&) = delete;
  WdTagger &operator=(WdTagger &&) = delete;

  [[nodiscard]] TaggerResult tag_file(const std::filesystem::path &path) const;

  [[nodiscard]] TaggerResult tag_bytes(std::span<const std::byte> bytes) const;

private:
  std::string endpoint_;
  // wd14 general / character 共用的置信度阈值，随每次打标请求发送。
  float threshold_;
  std::chrono::milliseconds timeout_;
  trantor::EventLoopThread loop_thread_;
  drogon::HttpClientPtr client_;
};

} // namespace tagimg
