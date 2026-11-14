#include "http/json_helpers.hpp"

#include <utility>

namespace tagimg {

drogon::HttpResponsePtr json_response(Json body,
                                      drogon::HttpStatusCode status) {
  auto response = drogon::HttpResponse::newHttpResponse();
  response->setStatusCode(status);
  response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  response->setBody(body.dump());
  return response;
}

Json envelope(const std::optional<std::string> &request_id, Json data,
              Json errors) {
  Json body;
  if (request_id) {
    body["requestId"] = *request_id;
  }
  body["data"] = std::move(data);
  body["errors"] = std::move(errors);
  return body;
}

Json error_object(std::string code, std::string message, Json path,
                  Json extensions) {
  Json error;
  error["code"] = std::move(code);
  error["message"] = std::move(message);
  if (!path.is_null()) {
    error["path"] = std::move(path);
  }
  if (!extensions.empty()) {
    error["extensions"] = std::move(extensions);
  }
  return error;
}

Json single_error(Json error) {
  Json errors = Json::array();
  errors.push_back(std::move(error));
  return errors;
}

} // namespace tagimg
