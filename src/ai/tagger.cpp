#include "ai/tagger.hpp"

#include <fstream>
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
        "tagger endpoint must start with http:// or https://");
  }

  const auto scheme_end = endpoint.find("://");
  if (endpoint.find('/', scheme_end + 3) != std::string::npos) {
    throw std::invalid_argument("tagger endpoint must not contain a path");
  }
  return endpoint;
}

bool parse_tags(const nlohmann::json &root, const char *key,
                std::vector<TagScore> &output) {
  const auto tags = root.find(key);
  if (tags == root.end() || !tags->is_array()) {
    return false;
  }

  std::vector<TagScore> parsed;
  parsed.reserve(tags->size());
  for (const auto &tag : *tags) {
    if (!tag.is_object()) {
      return false;
    }
    const auto name = tag.find("name");
    const auto score = tag.find("score");
    if (name == tag.end() || score == tag.end() || !name->is_string() ||
        !score->is_number()) {
      return false;
    }
    parsed.push_back({name->get<std::string>(), score->get<float>()});
  }
  output = std::move(parsed);
  return true;
}

} // namespace

std::string_view to_string(TaggerError error) noexcept {
  switch (error) {
  case TaggerError::SidecarUnavailable:
    return "SidecarUnavailable";
  case TaggerError::Timeout:
    return "Timeout";
  case TaggerError::InvalidImage:
    return "InvalidImage";
  case TaggerError::ImageTooLarge:
    return "ImageTooLarge";
  case TaggerError::HttpError:
    return "HttpError";
  case TaggerError::InvalidResponse:
    return "InvalidResponse";
  case TaggerError::FileReadError:
    return "FileReadError";
  }
  return "Unknown";
}

WdTagger::WdTagger(std::string endpoint, std::chrono::milliseconds timeout)
    : endpoint_(normalize_endpoint(std::move(endpoint))), timeout_(timeout),
      loop_thread_("tagimg-ai-client") {
  if (timeout_.count() <= 0) {
    throw std::invalid_argument("tagger timeout must be positive");
  }

  loop_thread_.run();
  client_ =
      drogon::HttpClient::newHttpClient(endpoint_, loop_thread_.getLoop());
  if (!client_) {
    throw std::runtime_error("failed to create tagger HTTP client");
  }
}

TaggerResult WdTagger::tag_file(const std::filesystem::path &path) const {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return TaggerResult::err(TaggerError::FileReadError);
  }
  if (size > kMaxImageBytes) {
    return TaggerResult::err(TaggerError::ImageTooLarge);
  }
  if (size == 0) {
    return TaggerResult::err(TaggerError::InvalidImage);
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return TaggerResult::err(TaggerError::FileReadError);
  }

  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
    return TaggerResult::err(TaggerError::FileReadError);
  }
  return tag_bytes(bytes);
}

TaggerResult WdTagger::tag_bytes(std::span<const std::byte> bytes) const {
  if (bytes.empty()) {
    return TaggerResult::err(TaggerError::InvalidImage);
  }
  if (bytes.size() > kMaxImageBytes) {
    return TaggerResult::err(TaggerError::ImageTooLarge);
  }
  if (!client_) {
    return TaggerResult::err(TaggerError::SidecarUnavailable);
  }

  auto request = drogon::HttpRequest::newHttpRequest();
  request->setMethod(drogon::Post);
  request->setPath("/v1/tag/image");
  request->setContentTypeCode(drogon::CT_APPLICATION_OCTET_STREAM);
  request->setBody(
      std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size()));

  const double timeout_seconds =
      std::chrono::duration<double>(timeout_).count();
  const auto [request_result, response] =
      client_->sendRequest(request, timeout_seconds);

  switch (request_result) {
  case drogon::ReqResult::Ok:
    break;
  case drogon::ReqResult::Timeout:
    return TaggerResult::err(TaggerError::Timeout);
  case drogon::ReqResult::BadServerAddress:
  case drogon::ReqResult::NetworkFailure:
    return TaggerResult::err(TaggerError::SidecarUnavailable);
  case drogon::ReqResult::BadResponse:
    return TaggerResult::err(TaggerError::InvalidResponse);
  case drogon::ReqResult::HandshakeError:
  case drogon::ReqResult::InvalidCertificate:
  case drogon::ReqResult::EncryptionFailure:
    return TaggerResult::err(TaggerError::HttpError);
  }

  if (!response) {
    return TaggerResult::err(TaggerError::InvalidResponse);
  }

  const int status = static_cast<int>(response->statusCode());
  if (status == 400) {
    return TaggerResult::err(TaggerError::InvalidImage);
  }
  if (status == 413) {
    return TaggerResult::err(TaggerError::ImageTooLarge);
  }
  if (status == 503) {
    return TaggerResult::err(TaggerError::SidecarUnavailable);
  }
  if (status < 200 || status >= 300) {
    return TaggerResult::err(TaggerError::HttpError);
  }

  TaggingResult result;
  try {
    const std::string_view body = response->getBody();
    const nlohmann::json root = nlohmann::json::parse(body.begin(), body.end());
    if (!root.is_object() || !parse_tags(root, "general", result.general) ||
        !parse_tags(root, "character", result.character) ||
        !parse_tags(root, "rating", result.rating)) {
      return TaggerResult::err(TaggerError::InvalidResponse);
    }
  } catch (const nlohmann::json::exception &) {
    return TaggerResult::err(TaggerError::InvalidResponse);
  }

  return TaggerResult::ok(std::move(result));
}

} // namespace tagimg
