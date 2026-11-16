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

enum class EmbedderError {
  SidecarUnavailable,
  Timeout,
  InvalidImage,
  ImageTooLarge,
  InvalidText,
  HttpError,
  InvalidResponse,
  FileReadError,
  DimensionMismatch,
};

[[nodiscard]] std::string_view to_string(EmbedderError error) noexcept;

// 768 维、已 L2 归一化的向量；点积即余弦相似度。
struct Embedding {
  std::vector<float> values;
};

using EmbedderResult = Result<Embedding, EmbedderError>;
using TextEmbedderResult = Result<std::vector<Embedding>, EmbedderError>;

// 本地 Python sidecar 的 Chinese-CLIP 客户端封装。
//
// 与 WdTagger 同构：把原始图片字节 POST 到 sidecar 的
// POST /v1/embed/image（文本走 POST /v1/embed/text），再解析返回的 JSON。
// 返回的向量已在 Python 侧归一化，这里只做维度与数值校验。
class ClipEmbedder {
public:
  static constexpr std::size_t kEmbeddingDim = 768;
  static constexpr std::size_t kMaxImageBytes = 50 * 1024 * 1024;
  static constexpr std::size_t kMaxTextsPerRequest = 32;

  explicit ClipEmbedder(
      std::string endpoint = "http://127.0.0.1:8001",
      // CPU 上的 ViT-L/14 比 tagger 慢得多，默认给 60 秒。
      std::chrono::milliseconds timeout = std::chrono::seconds(60));

  ClipEmbedder(const ClipEmbedder &) = delete;
  ClipEmbedder &operator=(const ClipEmbedder &) = delete;
  ClipEmbedder(ClipEmbedder &&) = delete;
  ClipEmbedder &operator=(ClipEmbedder &&) = delete;

  [[nodiscard]] EmbedderResult
  embed_file(const std::filesystem::path &path) const;

  [[nodiscard]] EmbedderResult
  embed_bytes(std::span<const std::byte> bytes) const;

  [[nodiscard]] TextEmbedderResult
  embed_texts(const std::vector<std::string> &texts) const;

  // 单条文本的便捷重载。
  [[nodiscard]] EmbedderResult embed_text(std::string_view text) const;

private:
  std::string endpoint_;
  std::chrono::milliseconds timeout_;
  trantor::EventLoopThread loop_thread_;
  drogon::HttpClientPtr client_;
};

} // namespace tagimg
