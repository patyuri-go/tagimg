#include "http/api_service.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <drogon/HttpAppFramework.h>
#include <nlohmann/json.hpp>

#include "common/uuid.hpp"
#include "common/validation.hpp"
#include "http/json_helpers.hpp"
#include "job/job.hpp"
#include "job/service_runtime.hpp"

namespace tagimg {
namespace {

constexpr std::size_t kMaxBodyBytes = 1024 * 1024;
constexpr double kRequestTimeoutSeconds = 300.0;

// 一次 addArtwork 请求的响应上下文。
//
// done 保证超时和 job 完成只有一方能真正写回响应；callback 必须在 Drogon
// IO 线程里调用，因此最终响应统一通过 runInLoop() 投递。
struct PendingResponse {
  std::function<void(const drogon::HttpResponsePtr &)> callback;
  std::atomic<bool> done{false};
  std::optional<std::string> request_id;
  std::chrono::steady_clock::time_point started;
  std::uint64_t timeout_id = 0;
};

Json variable_path(std::string_view field,
                   std::optional<std::size_t> index = std::nullopt) {
  Json path = Json::array();
  path.push_back("variables");
  path.push_back(field);
  if (index) {
    path.push_back(*index);
  }
  return path;
}

const char *job_error_code(JobError error) {
  switch (error) {
  case JobError::InvalidArgument:
    return "INVALID_ARGUMENT";
  case JobError::InvalidEntityId:
    return "INVALID_ENTITY_ID";
  case JobError::EntityNotFound:
    return "ENTITY_NOT_FOUND";
  case JobError::NotImage:
    return "NOT_IMAGE";
  case JobError::RepositoryFailed:
    return "REPOSITORY_FAILED";
  case JobError::DownloadFailed:
    return "DOWNLOAD_FAILED";
  case JobError::FileError:
    return "FILE_ERROR";
  case JobError::ImageTooLarge:
    return "IMAGE_TOO_LARGE";
  case JobError::AiUnavailable:
    return "AI_UNAVAILABLE";
  case JobError::TaggingFailed:
    return "TAGGING_FAILED";
  case JobError::DuplicateImage:
    return "DUPLICATE_IMAGE";
  case JobError::UnknownTag:
    return "UNKNOWN_TAG";
  case JobError::UnknownArtist:
    return "UNKNOWN_ARTIST";
  case JobError::TagHierarchyMissing:
    return "TAG_HIERARCHY_MISSING";
  case JobError::WriteFailed:
    return "WRITE_FAILED";
  case JobError::InternalError:
  case JobError::JobNotFinished:
    return "INTERNAL_ERROR";
  }
  return "INTERNAL_ERROR";
}

bool job_error_retryable(JobError error) {
  switch (error) {
  case JobError::RepositoryFailed:
  case JobError::DownloadFailed:
  case JobError::AiUnavailable:
  case JobError::TaggingFailed:
  case JobError::WriteFailed:
  case JobError::InternalError:
  case JobError::JobNotFinished:
    return true;
  default:
    return false;
  }
}

std::string job_error_message(JobError error) {
  switch (error) {
  case JobError::InvalidArgument:
    return "invalid argument";
  case JobError::InvalidEntityId:
    return "entity id is not a valid uuid";
  case JobError::EntityNotFound:
    return "entity not found";
  case JobError::NotImage:
    return "entity is not an image";
  case JobError::RepositoryFailed:
    return "entity repository query failed";
  case JobError::DownloadFailed:
    return "failed to download entity";
  case JobError::FileError:
    return "temporary file error";
  case JobError::ImageTooLarge:
    return "image exceeds the size limit";
  case JobError::AiUnavailable:
    return "AI service is unavailable or models are not ready";
  case JobError::TaggingFailed:
    return "image tagging failed";
  case JobError::DuplicateImage:
    return "one of the entities is already in the images table";
  case JobError::UnknownTag:
    return "tag id does not exist";
  case JobError::UnknownArtist:
    return "artist id does not exist";
  case JobError::TagHierarchyMissing:
    return "wd14scan tag hierarchy is missing";
  case JobError::WriteFailed:
    return "database write failed";
  case JobError::InternalError:
    return "internal error";
  case JobError::JobNotFinished:
    return "job is not finished";
  }
  return "internal error";
}

struct ValidationError {
  std::string message;
  Json path;
};

bool is_known_variable(std::string_view name) {
  static constexpr std::array<std::string_view, 6> kNames = {
      "entityIds",   "title",  "description",
      "publishedAt", "tagIds", "artistIds"};
  return std::find(kNames.begin(), kNames.end(), name) != kNames.end();
}

std::optional<ValidationError>
read_uuid_array(const Json &variables, const std::string &key, bool required,
                bool reject_duplicates, std::vector<std::string> &output) {
  const auto it = variables.find(key);
  if (it == variables.end()) {
    if (required) {
      return ValidationError{key + " is required", variable_path(key)};
    }
    return std::nullopt;
  }
  if (!it->is_array()) {
    return ValidationError{key + " must be an array", variable_path(key)};
  }

  for (std::size_t index = 0; index < it->size(); ++index) {
    const Json &item = (*it)[index];
    if (!item.is_string()) {
      return ValidationError{key + " must contain uuid strings",
                             variable_path(key, index)};
    }
    std::string value = item.get<std::string>();
    if (!is_uuid(value)) {
      return ValidationError{key + " contains an invalid uuid",
                             variable_path(key, index)};
    }
    output.push_back(std::move(value));
  }

  if (reject_duplicates) {
    std::vector<std::string> sorted = output;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
      return ValidationError{key + " contains duplicate items",
                             variable_path(key)};
    }
  }
  return std::nullopt;
}

std::optional<ValidationError> validate_add_request(const Json &variables,
                                                    AddRequest &request) {
  if (!variables.is_object()) {
    Json path = Json::array();
    path.push_back("variables");
    return ValidationError{"variables must be an object", std::move(path)};
  }

  for (auto it = variables.begin(); it != variables.end(); ++it) {
    if (!is_known_variable(it.key())) {
      return ValidationError{"unknown variable: " + it.key(),
                             variable_path(it.key())};
    }
  }

  if (auto error = read_uuid_array(variables, "entityIds", true, true,
                                   request.entity_ids)) {
    return error;
  }
  if (request.entity_ids.empty()) {
    return ValidationError{"entityIds must not be empty",
                           variable_path("entityIds")};
  }
  if (request.entity_ids.size() > kMaxEntityIds) {
    return ValidationError{"entityIds must contain at most " +
                               std::to_string(kMaxEntityIds) + " items",
                           variable_path("entityIds")};
  }

  const auto title = variables.find("title");
  if (title == variables.end()) {
    return ValidationError{"title is required", variable_path("title")};
  }
  if (!title->is_string()) {
    return ValidationError{"title must be a string", variable_path("title")};
  }
  request.title = title->get<std::string>();
  if (request.title.empty()) {
    return ValidationError{"title must not be empty", variable_path("title")};
  }
  if (utf8_length(request.title) > kMaxTitleLength) {
    return ValidationError{"title must be at most " +
                               std::to_string(kMaxTitleLength) + " characters",
                           variable_path("title")};
  }

  const auto description = variables.find("description");
  if (description != variables.end() && !description->is_null()) {
    if (!description->is_string()) {
      return ValidationError{"description must be a string or null",
                             variable_path("description")};
    }
    request.description = description->get<std::string>();
  }

  const auto published = variables.find("publishedAt");
  if (published != variables.end() && !published->is_null()) {
    if (!published->is_string()) {
      return ValidationError{"publishedAt must be a string or null",
                             variable_path("publishedAt")};
    }
    std::string value = published->get<std::string>();
    if (!has_timestamp_shape(value)) {
      return ValidationError{"publishedAt must be an RFC3339 timestamp",
                             variable_path("publishedAt")};
    }
    request.published_at = std::move(value);
  }

  if (auto error =
          read_uuid_array(variables, "tagIds", false, false, request.tag_ids)) {
    return error;
  }
  if (auto error = read_uuid_array(variables, "artistIds", false, false,
                                   request.artist_ids)) {
    return error;
  }
  return std::nullopt;
}

Json add_artwork_data(const AddResult &add, double elapsed_ms) {
  Json artwork;
  artwork["artworkId"] = add.artwork_id;
  artwork["imageIds"] = add.image_ids;
  artwork["scannedTagCount"] = add.tag_count;
  artwork["elapsedMs"] = elapsed_ms;

  Json data;
  data["addArtwork"] = std::move(artwork);
  return data;
}

Json failure_errors(const AddResult &add) {
  Json errors = Json::array();
  for (const ScanFailure &failure : add.failures) {
    Json path = Json::array();
    path.push_back("variables");
    path.push_back("entityIds");
    const auto found =
        std::find(add.request.entity_ids.begin(), add.request.entity_ids.end(),
                  failure.entity_id);
    if (found != add.request.entity_ids.end()) {
      path.push_back(static_cast<std::size_t>(
          std::distance(add.request.entity_ids.begin(), found)));
    }

    Json extensions;
    extensions["entityId"] = failure.entity_id;
    extensions["retryable"] = job_error_retryable(failure.error);

    errors.push_back(error_object(job_error_code(failure.error),
                                  job_error_message(failure.error),
                                  std::move(path), std::move(extensions)));
  }
  return errors;
}

Json schema_field(std::string name, std::string type, bool required,
                  std::string description) {
  Json field;
  field["name"] = std::move(name);
  field["type"] = std::move(type);
  field["required"] = required;
  field["description"] = std::move(description);
  return field;
}

} // namespace

struct ApiService::Impl {
  // 扫描任务由 GraphQL 与 REST 共用，生命周期由 main 控制。
  std::shared_ptr<ServiceRuntime> runtime;
};

ApiService::ApiService(std::shared_ptr<ServiceRuntime> runtime)
    : impl_(std::make_shared<Impl>()) {
  if (!runtime) {
    throw std::invalid_argument("ApiService requires a ServiceRuntime");
  }
  impl_->runtime = std::move(runtime);
}

ApiService::~ApiService() {
  shutdown();
}

void ApiService::shutdown() {
  if (!impl_ || !impl_->runtime) {
    return;
  }
  impl_->runtime->shutdown();
}

void ApiService::handle_rpc(
    const drogon::HttpRequestPtr &request,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) const {
  std::string content_type = request->getHeader("content-type");
  std::transform(
      content_type.begin(), content_type.end(), content_type.begin(),
      [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
  if (content_type.rfind("application/json", 0) != 0) {
    callback(
        json_response(envelope(std::nullopt, nullptr,
                               single_error(error_object(
                                   "UNSUPPORTED_MEDIA_TYPE",
                                   "Content-Type must be application/json"))),
                      drogon::k415UnsupportedMediaType));
    return;
  }

  Json root;
  try {
    root = Json::parse(request->body());
  } catch (const Json::exception &) {
    callback(json_response(
        envelope(std::nullopt, nullptr,
                 single_error(error_object("INVALID_JSON",
                                           "request body is not valid json"))),
        drogon::k400BadRequest));
    return;
  }

  if (!root.is_object()) {
    callback(json_response(
        envelope(std::nullopt, nullptr,
                 single_error(error_object("INVALID_REQUEST",
                                           "request body must be an object"))),
        drogon::k400BadRequest));
    return;
  }

  std::optional<std::string> request_id;
  const auto request_id_it = root.find("requestId");
  if (request_id_it != root.end()) {
    if (!request_id_it->is_string()) {
      callback(json_response(
          envelope(std::nullopt, nullptr,
                   single_error(error_object("INVALID_REQUEST",
                                             "requestId must be a string"))),
          drogon::k400BadRequest));
      return;
    }
    request_id = request_id_it->get<std::string>();
  }

  const auto operation_it = root.find("operation");
  if (operation_it == root.end() || !operation_it->is_string() ||
      operation_it->get_ref<const std::string &>().empty()) {
    callback(json_response(
        envelope(request_id, nullptr,
                 single_error(
                     error_object("INVALID_REQUEST", "operation is required"))),
        drogon::k400BadRequest));
    return;
  }

  const auto variables_it = root.find("variables");
  if (variables_it == root.end() || !variables_it->is_object()) {
    callback(json_response(
        envelope(request_id, nullptr,
                 single_error(error_object("INVALID_REQUEST",
                                           "variables must be an object"))),
        drogon::k400BadRequest));
    return;
  }

  const std::string operation = operation_it->get<std::string>();
  if (operation != "addArtwork") {
    callback(json_response(envelope(
        request_id, nullptr,
        single_error(error_object("UNKNOWN_OPERATION",
                                  "unknown operation: " + operation)))));
    return;
  }

  AddRequest add_request;
  if (const auto error = validate_add_request(*variables_it, add_request)) {
    callback(json_response(
        envelope(request_id, nullptr,
                 single_error(error_object("INVALID_ARGUMENT", error->message,
                                           error->path)))));
    return;
  }

  auto pending = std::make_shared<PendingResponse>();
  pending->callback = std::move(callback);
  pending->request_id = request_id;
  pending->started = std::chrono::steady_clock::now();

  const bool accepted = impl_->runtime->submit(
      std::move(add_request),
      [pending](const Result<JobOutcome, JobError> &result) {
        Json body;
        if (!result) {
          Json extensions;
          extensions["retryable"] = job_error_retryable(result.error());
          body = envelope(
              pending->request_id, nullptr,
              single_error(error_object(job_error_code(result.error()),
                                        job_error_message(result.error()),
                                        Json(), std::move(extensions))));
        } else if (const auto *add = std::get_if<AddResult>(&*result)) {
          if (!add->failures.empty()) {
            body = envelope(pending->request_id, nullptr, failure_errors(*add));
          } else {
            const double elapsed_ms =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - pending->started)
                    .count();
            body = envelope(pending->request_id,
                            add_artwork_data(*add, elapsed_ms), Json::array());
          }
        } else {
          body = envelope(pending->request_id, nullptr,
                          single_error(error_object("INTERNAL_ERROR",
                                                    "unexpected job outcome")));
        }

        auto response = json_response(std::move(body));
        drogon::app().getLoop()->runInLoop([pending, response] {
          if (pending->done.exchange(true)) {
            return;
          }
          drogon::app().getLoop()->invalidateTimer(pending->timeout_id);
          pending->callback(response);
        });
      });

  if (!accepted) {
    pending->done.store(true);
    pending->callback(json_response(
        envelope(request_id, nullptr,
                 single_error(error_object("SERVICE_UNAVAILABLE",
                                           "job worker is not running", Json(),
                                           {{"retryable", true}}))),
        drogon::k503ServiceUnavailable));
    return;
  }

  pending->timeout_id =
      drogon::app().getLoop()->runAfter(kRequestTimeoutSeconds, [pending] {
        if (pending->done.exchange(true)) {
          return;
        }
        Json extensions;
        extensions["retryable"] = true;
        auto response = json_response(envelope(
            pending->request_id, nullptr,
            single_error(error_object("TIMEOUT", "addArtwork timed out", Json(),
                                      std::move(extensions)))));
        pending->callback(response);
      });
}

void ApiService::handle_schema(
    const drogon::HttpRequestPtr &,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) const {
  Json variables = Json::array();
  variables.push_back(schema_field("entityIds", "[UUID]", true,
                                   "图片实体 ID，1 到 " +
                                       std::to_string(kMaxEntityIds) + " 个"));
  variables.push_back(
      schema_field("title", "string", true, "作品标题，1 到 500 个字符"));
  variables.push_back(schema_field("description", "string|null", false,
                                   "作品描述，缺省或 null 写入 NULL"));
  variables.push_back(
      schema_field("publishedAt", "string|null", false,
                   "RFC3339 原始发布时间，缺省或 null 写入 NULL"));
  variables.push_back(schema_field("tagIds", "[UUID]", false,
                                   "已存在的标签 ID，直接挂到作品上"));
  variables.push_back(schema_field("artistIds", "[UUID]", false,
                                   "已存在的艺术家 ID，写入 artwork_artists"));

  Json operation;
  operation["name"] = "addArtwork";
  operation["summary"] = "扫描图片实体并新建作品";
  operation["variables"] = std::move(variables);
  operation["errors"] = Json::array(
      {"INVALID_ARGUMENT", "INVALID_ENTITY_ID", "ENTITY_NOT_FOUND", "NOT_IMAGE",
       "IMAGE_TOO_LARGE", "REPOSITORY_FAILED", "DOWNLOAD_FAILED", "FILE_ERROR",
       "AI_UNAVAILABLE", "TAGGING_FAILED", "DUPLICATE_IMAGE", "UNKNOWN_TAG",
       "UNKNOWN_ARTIST", "TAG_HIERARCHY_MISSING", "WRITE_FAILED",
       "INTERNAL_ERROR", "TIMEOUT"});

  Json resource;
  resource["method"] = "GET";
  resource["path"] = "/api/v1/image/{imageId}";
  resource["description"] =
      "按图片 ID 返回图片；size=original 时 proxy 模式流式返回原始字节、"
      "redirect 模式返回 302 指向存储服务的内容地址；其他 size 预设由"
      "服务端等比缩放并编码为 AVIF。GIF 和动态 WebP 会保留动画帧并转码为"
      "动图 AVIF；APNG 等多帧格式无法通过 libvips 解码，按首帧静态图处理。";
  resource["body"] = "binary";
  resource["parameters"] = Json::array(
      {{{"name", "size"},
        {"in", "query"},
        {"required", false},
        {"type", "string"},
        {"enum",
         Json::array({"mini", "thumb", "small", "regular", "original"})},
        {"default", "regular"},
        {"description",
         "输出尺寸预设：mini 48px、thumb 250px、small 540px、regular 长边 "
         "1200px、original 原始字节；除 original 外均转码为 AVIF，动图保留"
         "动画（APNG 仅首帧）"}},
       {{"name", "download"},
        {"in", "query"},
        {"required", false},
        {"description",
         "为 1 时使用 Content-Disposition: attachment 并强制 original（忽略 "
         "size）；redirect 模式仍可能返回 302，不保证 attachment"}},
       {{"name", "requestId"},
        {"in", "query"},
        {"required", false},
        {"description", "回显到错误响应"}}});
  resource["errors"] = Json::array(
      {"INVALID_ARGUMENT", "IMAGE_NOT_FOUND", "ENTITY_NOT_FOUND",
       "STORAGE_NOT_FOUND", "NOT_IMAGE", "IMAGE_TOO_LARGE",
       "IMAGE_PROCESSING_FAILED", "IMAGE_PROCESSING_TIMEOUT",
       "REPOSITORY_FAILED", "STORAGE_UNAVAILABLE", "SERVICE_UNAVAILABLE"});

  Json body;
  body["transport"] = {{"method", "POST"},
                       {"path", "/api/v1"},
                       {"contentType", "application/json"},
                       {"maxBodyBytes", kMaxBodyBytes}};
  body["schemaPath"] = "/api/v1/schema";
  body["operations"] = Json::array({std::move(operation)});
  body["resources"] = Json::array({std::move(resource)});
  callback(json_response(std::move(body)));
}

} // namespace tagimg
