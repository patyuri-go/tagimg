#include "ai/health.hpp"

#include <stdexcept>
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
        "ai endpoint must start with http:// or https://");
  }

  const auto scheme_end = endpoint.find("://");
  if (endpoint.find('/', scheme_end + 3) != std::string::npos) {
    throw std::invalid_argument("ai endpoint must not contain a path");
  }
  return endpoint;
}

} // namespace

std::string_view to_string(AiHealthError error) noexcept {
  switch (error) {
  case AiHealthError::Unavailable:
    return "Unavailable";
  case AiHealthError::Timeout:
    return "Timeout";
  case AiHealthError::InvalidResponse:
    return "InvalidResponse";
  }
  return "Unknown";
}

AiHealthClient::AiHealthClient(std::string endpoint,
                               std::chrono::milliseconds timeout)
    : endpoint_(normalize_endpoint(std::move(endpoint))), timeout_(timeout),
      loop_thread_("tagimg-ai-health") {
  if (timeout_.count() <= 0) {
    throw std::invalid_argument("ai health timeout must be positive");
  }

  loop_thread_.run();
  client_ = drogon::HttpClient::newHttpClient(endpoint_, loop_thread_.getLoop());
  if (!client_) {
    throw std::runtime_error("failed to create ai health HTTP client");
  }
}

AiHealthResult AiHealthClient::check() const {
  if (!client_) {
    return AiHealthResult::err(AiHealthError::Unavailable);
  }

  auto request = drogon::HttpRequest::newHttpRequest();
  request->setMethod(drogon::Get);
  request->setPath("/health");

  const double timeout_seconds =
      std::chrono::duration<double>(timeout_).count();
  const auto [request_result, response] =
      client_->sendRequest(request, timeout_seconds);

  switch (request_result) {
  case drogon::ReqResult::Ok:
    break;
  case drogon::ReqResult::Timeout:
    return AiHealthResult::err(AiHealthError::Timeout);
  case drogon::ReqResult::BadServerAddress:
  case drogon::ReqResult::NetworkFailure:
  case drogon::ReqResult::HandshakeError:
  case drogon::ReqResult::InvalidCertificate:
  case drogon::ReqResult::EncryptionFailure:
    return AiHealthResult::err(AiHealthError::Unavailable);
  case drogon::ReqResult::BadResponse:
    return AiHealthResult::err(AiHealthError::InvalidResponse);
  }

  if (!response) {
    return AiHealthResult::err(AiHealthError::InvalidResponse);
  }
  if (response->statusCode() != drogon::k200OK) {
    // tagger 未加载时 Python 返回 503，等价于 AI 不可用。
    return AiHealthResult::err(AiHealthError::Unavailable);
  }

  try {
    const std::string_view body = response->getBody();
    const nlohmann::json root = nlohmann::json::parse(body.begin(), body.end());
    if (!root.is_object()) {
      return AiHealthResult::err(AiHealthError::InvalidResponse);
    }

    const auto loaded = root.find("loaded");
    if (loaded == root.end() || !loaded->is_boolean()) {
      return AiHealthResult::err(AiHealthError::InvalidResponse);
    }

    const auto clip = root.find("clip");
    if (clip == root.end() || !clip->is_object()) {
      return AiHealthResult::err(AiHealthError::InvalidResponse);
    }
    const auto clip_loaded = clip->find("loaded");
    if (clip_loaded == clip->end() || !clip_loaded->is_boolean()) {
      return AiHealthResult::err(AiHealthError::InvalidResponse);
    }

    AiHealthStatus status;
    status.tagger_loaded = loaded->get<bool>();
    status.clip_loaded = clip_loaded->get<bool>();
    return AiHealthResult::ok(status);
  } catch (const nlohmann::json::exception &) {
    return AiHealthResult::err(AiHealthError::InvalidResponse);
  }
}

} // namespace tagimg
