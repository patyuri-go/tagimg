#pragma once

#include <optional>
#include <string>

#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>

namespace tagimg {

using Json = nlohmann::json;

// 统一的 JSON 响应与错误信封构造。
//
// RPC 端点（POST /api/v1）和图片流端点共用同一套错误格式，所以抽出来
// 放在这里，避免两处各写一份。
[[nodiscard]] drogon::HttpResponsePtr
json_response(Json body, drogon::HttpStatusCode status = drogon::k200OK);

// {"requestId": ..., "data": ..., "errors": ...}
[[nodiscard]] Json envelope(const std::optional<std::string> &request_id,
                            Json data, Json errors);

// path/extensions 为空时不写对应字段。
[[nodiscard]] Json error_object(std::string code, std::string message,
                                Json path = Json(),
                                Json extensions = Json::object());

[[nodiscard]] Json single_error(Json error);

} // namespace tagimg
