#include "ai/clip_embedder.hpp"

#include <cmath>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace tagimg {
namespace {

std::string normalize_endpoint(std::string endpoint) {
  while (!endpoint.empty() && endpoint.back() == '/') {
    endpoint.pop_back();
  }
  if (endpoint.rfind("http://", 0) != 0 && endpoint.rfind("https://", 0) != 0) {
    throw std::invalid_argument(
        "embedder endpoint must start with http:// or https://");
  }

  const auto scheme_end = endpoint.find("://");
  if (endpoint.find('/', scheme_end + 3) != std::string::npos) {
    throw std::invalid_argument("embedder endpoint must not contain a path");
  }
  return endpoint;
}

// sidecar 说归一化过，这里再抽检一次，防止配置错误把脏向量写进库。
bool looks_normalized(const std::vector<float> &values) {
  double sum = 0.0;
  for (const float value : values) {
    sum += static_cast<double>(value) * static_cast<double>(value);
  }
  const double norm = std::sqrt(sum);
  return std::abs(norm - 1.0) < 1.0e-2;
}

bool parse_embedding(const nlohmann::json &node, Embedding &output) {
  if (!node.is_array()) {
    return false;
  }
  if (node.size() != ClipEmbedder::kEmbeddingDim) {
    return false;
  }

  std::vector<float> values;
  values.reserve(node.size());
  for (const auto &item : node) {
    if (!item.is_number()) {
      return false;
    }
    const double raw = item.get<double>();
    if (!std::isfinite(raw)) {
      return false;
    }
    values.push_back(static_cast<float>(raw));
  }
  if (!looks_normalized(values)) {
    return false;
  }
  output.values = std::move(values);
  return true;
}

// 统一处理 HTTP 层结果，返回空表示可以继续解析响应体。
std::optional<EmbedderError>
map_request_result(drogon::ReqResult result) {
  switch (result) {
  case drogon::ReqResult::Ok:
    return std::nullopt;
  case drogon::ReqResult::Timeout:
    return EmbedderError::Timeout;
  case drogon::ReqResult::BadServerAddress:
  case drogon::ReqResult::NetworkFailure:
    return EmbedderError::SidecarUnavailable;
  case drogon::ReqResult::BadResponse:
    return EmbedderError::InvalidResponse;
  case drogon::ReqResult::HandshakeError:
  case drogon::ReqResult::InvalidCertificate:
  case drogon::ReqResult::EncryptionFailure:
    return EmbedderError::HttpError;
  }
  return EmbedderError::HttpError;
}

} // namespace

std::string_view to_string(EmbedderError error) noexcept {
  switch (error) {
  case EmbedderError::SidecarUnavailable:
    return "SidecarUnavailable";
  case EmbedderError::Timeout:
    return "Timeout";
  case EmbedderError::InvalidImage:
    return "InvalidImage";
  case EmbedderError::ImageTooLarge:
    return "ImageTooLarge";
  case EmbedderError::InvalidText:
    return "InvalidText";
  case EmbedderError::HttpError:
    return "HttpError";
  case EmbedderError::InvalidResponse:
    return "InvalidResponse";
  case EmbedderError::FileReadError:
    return "FileReadError";
  case EmbedderError::DimensionMismatch:
    return "DimensionMismatch";
  }
  return "Unknown";
}

ClipEmbedder::ClipEmbedder(std::string endpoint, std::chrono::milliseconds timeout)
    : endpoint_(normalize_endpoint(std::move(endpoint))), timeout_(timeout),
      loop_thread_("tagimg-clip-client") {
  if (timeout_.count() <= 0) {
    throw std::invalid_argument("embedder timeout must be positive");
  }

  loop_thread_.run();
  client_ =
      drogon::HttpClient::newHttpClient(endpoint_, loop_thread_.getLoop());
  if (!client_) {
    throw std::runtime_error("failed to create embedder HTTP client");
  }
}

EmbedderResult ClipEmbedder::embed_file(const std::filesystem::path &path) const {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return EmbedderResult::err(EmbedderError::FileReadError);
  }
  if (size > kMaxImageBytes) {
    return EmbedderResult::err(EmbedderError::ImageTooLarge);
  }
  if (size == 0) {
    return EmbedderResult::err(EmbedderError::InvalidImage);
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return EmbedderResult::err(EmbedderError::FileReadError);
  }

  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
    return EmbedderResult::err(EmbedderError::FileReadError);
  }
  return embed_bytes(bytes);
}

EmbedderResult
ClipEmbedder::embed_bytes(std::span<const std::byte> bytes) const {
  if (bytes.empty()) {
    return EmbedderResult::err(EmbedderError::InvalidImage);
  }
  if (bytes.size() > kMaxImageBytes) {
    return EmbedderResult::err(EmbedderError::ImageTooLarge);
  }
  if (!client_) {
    return EmbedderResult::err(EmbedderError::SidecarUnavailable);
  }

  auto request = drogon::HttpRequest::newHttpRequest();
  request->setMethod(drogon::Post);
  request->setPath("/v1/embed/image");
  request->setContentTypeCode(drogon::CT_APPLICATION_OCTET_STREAM);
  request->setBody(
      std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size()));

  const double timeout_seconds =
      std::chrono::duration<double>(timeout_).count();
  const auto [request_result, response] =
      client_->sendRequest(request, timeout_seconds);

  if (const auto failure = map_request_result(request_result)) {
    return EmbedderResult::err(*failure);
  }
  if (!response) {
    return EmbedderResult::err(EmbedderError::InvalidResponse);
  }

  const int status = static_cast<int>(response->statusCode());
  if (status == 400) {
    return EmbedderResult::err(EmbedderError::InvalidImage);
  }
  if (status == 413) {
    return EmbedderResult::err(EmbedderError::ImageTooLarge);
  }
  if (status == 503) {
    return EmbedderResult::err(EmbedderError::SidecarUnavailable);
  }
  if (status < 200 || status >= 300) {
    return EmbedderResult::err(EmbedderError::HttpError);
  }

  Embedding embedding;
  try {
    const std::string_view body = response->getBody();
    const nlohmann::json root = nlohmann::json::parse(body.begin(), body.end());
    if (!root.is_object()) {
      return EmbedderResult::err(EmbedderError::InvalidResponse);
    }

    const auto dim = root.find("dim");
    if (dim != root.end() && dim->is_number() &&
        dim->get<std::size_t>() != kEmbeddingDim) {
      return EmbedderResult::err(EmbedderError::DimensionMismatch);
    }

    const auto values = root.find("embedding");
    if (values == root.end()) {
      return EmbedderResult::err(EmbedderError::InvalidResponse);
    }
    if (!parse_embedding(*values, embedding)) {
      // 长度不对单独区分，便于排查模型/接口不匹配。
      return EmbedderResult::err(
          values->is_array() && values->size() != kEmbeddingDim
              ? EmbedderError::DimensionMismatch
              : EmbedderError::InvalidResponse);
    }
  } catch (const nlohmann::json::exception &) {
    return EmbedderResult::err(EmbedderError::InvalidResponse);
  }

  return EmbedderResult::ok(std::move(embedding));
}

TextEmbedderResult
ClipEmbedder::embed_texts(const std::vector<std::string> &texts) const {
  if (texts.empty() || texts.size() > kMaxTextsPerRequest) {
    return TextEmbedderResult::err(EmbedderError::InvalidText);
  }
  if (!client_) {
    return TextEmbedderResult::err(EmbedderError::SidecarUnavailable);
  }

  nlohmann::json payload;
  payload["texts"] = texts;

  auto request = drogon::HttpRequest::newHttpRequest();
  request->setMethod(drogon::Post);
  request->setPath("/v1/embed/text");
  request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  request->setBody(payload.dump());

  const double timeout_seconds =
      std::chrono::duration<double>(timeout_).count();
  const auto [request_result, response] =
      client_->sendRequest(request, timeout_seconds);

  if (const auto failure = map_request_result(request_result)) {
    return TextEmbedderResult::err(*failure);
  }
  if (!response) {
    return TextEmbedderResult::err(EmbedderError::InvalidResponse);
  }

  const int status = static_cast<int>(response->statusCode());
  if (status == 400) {
    return TextEmbedderResult::err(EmbedderError::InvalidText);
  }
  if (status == 413) {
    return TextEmbedderResult::err(EmbedderError::ImageTooLarge);
  }
  if (status == 503) {
    return TextEmbedderResult::err(EmbedderError::SidecarUnavailable);
  }
  if (status < 200 || status >= 300) {
    return TextEmbedderResult::err(EmbedderError::HttpError);
  }

  std::vector<Embedding> embeddings;
  try {
    const std::string_view body = response->getBody();
    const nlohmann::json root = nlohmann::json::parse(body.begin(), body.end());
    if (!root.is_object()) {
      return TextEmbedderResult::err(EmbedderError::InvalidResponse);
    }

    const auto list = root.find("embeddings");
    if (list == root.end() || !list->is_array() ||
        list->size() != texts.size()) {
      return TextEmbedderResult::err(EmbedderError::InvalidResponse);
    }

    embeddings.reserve(list->size());
    for (const auto &node : *list) {
      Embedding embedding;
      if (!parse_embedding(node, embedding)) {
        return TextEmbedderResult::err(
            node.is_array() && node.size() != kEmbeddingDim
                ? EmbedderError::DimensionMismatch
                : EmbedderError::InvalidResponse);
      }
      embeddings.push_back(std::move(embedding));
    }
  } catch (const nlohmann::json::exception &) {
    return TextEmbedderResult::err(EmbedderError::InvalidResponse);
  }

  return TextEmbedderResult::ok(std::move(embeddings));
}

EmbedderResult ClipEmbedder::embed_text(std::string_view text) const {
  const auto result = embed_texts({std::string(text)});
  if (!result) {
    return EmbedderResult::err(result.error());
  }
  if (result->empty()) {
    return EmbedderResult::err(EmbedderError::InvalidResponse);
  }
  return EmbedderResult::ok(result->front());
}

} // namespace tagimg
