#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include <drogon/drogon.h>

#include "ai/clip_embedder.hpp"
#include "ai/sidecar_process.hpp"
#include "ai/tagger.hpp"
#include "common/config.hpp"
#include "db/entity_repository.hpp"
#include "db/tagimg_repository.hpp"
#include "graphql/graphql_service.hpp"
#include "http/api_service.hpp"
#include "http/image_stream.hpp"
#include "image/image_processor.hpp"
#include "job/service_runtime.hpp"

namespace {

std::string env_or(const char *name, std::string fallback) {
  const char *value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return fallback;
  }
  return value;
}

// 配置里的相对路径都按配置文件所在目录解析。
std::filesystem::path resolve_path(const std::filesystem::path &base,
                                   const std::filesystem::path &path) {
  if (path.is_absolute()) {
    return path;
  }
  return (base / path).lexically_normal();
}

tagimg::SidecarConfig make_sidecar_config(const tagimg::AiConfig &ai,
                                          const std::filesystem::path &base) {
  tagimg::SidecarConfig sidecar;
  sidecar.host = ai.host;
  sidecar.port = ai.port;
  sidecar.python = resolve_path(base, ai.python);
  sidecar.python_path = resolve_path(base, ai.python_path);
  sidecar.model_dir = resolve_path(base, ai.model_dir);
  sidecar.module = ai.module;
  sidecar.threads = ai.threads;
  sidecar.startup_timeout = std::chrono::seconds(ai.startup_timeout_seconds);
  sidecar.clip_model_dir = resolve_path(base, ai.clip_model_dir);
  sidecar.clip_threads = ai.clip_threads;
  sidecar.clip_preload = ai.clip_preload;
  sidecar.clip_startup_timeout =
      std::chrono::seconds(ai.clip_startup_timeout_seconds);
  return sidecar;
}

} // namespace

int main() {
  const std::filesystem::path config_path(
      env_or("TAGIMG_CONFIG", "config.json"));

  const auto config = tagimg::load_config(config_path);
  if (!config) {
    std::cerr << "config error: " << tagimg::to_string(config.error()) << '\n';
    return 2;
  }

  std::error_code path_error;
  const std::filesystem::path config_dir =
      std::filesystem::absolute(config_path, path_error).parent_path();

  tagimg::EntityDbConfig entity_db;
  entity_db.host = config->database.host;
  entity_db.port = config->database.port;
  entity_db.user = config->database.user;
  entity_db.password = config->database.password;
  entity_db.dbname = config->database.entity_dbname;

  auto entity_repository = tagimg::EntityRepository::connect(entity_db);
  if (!entity_repository) {
    std::cerr << "entity database error: "
              << tagimg::to_string(entity_repository.error()) << '\n';
    return 2;
  }

  tagimg::TagimgDbConfig tagimg_db;
  tagimg_db.host = config->database.host;
  tagimg_db.port = config->database.port;
  tagimg_db.user = config->database.user;
  tagimg_db.password = config->database.password;
  tagimg_db.dbname = config->database.image_dbname;

  auto tagimg_repository = tagimg::TagimgRepository::connect(tagimg_db);
  if (!tagimg_repository) {
    std::cerr << "tagimg database error: "
              << tagimg::to_string(tagimg_repository.error()) << '\n';
    return 2;
  }

  // sidecar 必须声明在 service 之前：析构顺序相反，先停 worker 再杀 Python。
  auto sidecar = tagimg::SidecarProcess::start(
      make_sidecar_config(config->ai, config_dir));
  if (!sidecar) {
    std::cerr << "sidecar error: " << tagimg::to_string(sidecar.error())
              << '\n';
    return 2;
  }
  const std::string endpoint = (*sidecar)->endpoint();
  std::cout << "sidecar " << endpoint << ' '
            << ((*sidecar)->managed() ? "spawned" : "reused") << std::endl;

  auto tagger = std::make_shared<tagimg::WdTagger>(endpoint);
  // CLIP 与 tagger 共用同一个 sidecar；CPU 上 ViT-L/14 明显更慢，超时单独配置。
  auto embedder = std::make_shared<tagimg::ClipEmbedder>(
      endpoint, std::chrono::seconds(config->ai.clip_timeout_seconds));
  const std::filesystem::path work_dir(
      env_or("TAGIMG_WORK_DIR",
             (std::filesystem::temp_directory_path() / "tagimg").string()));

  auto entity_repository_shared =
      std::shared_ptr<const tagimg::EntityRepository>(
          std::move(*entity_repository));
  auto tagimg_repository_shared =
      std::shared_ptr<const tagimg::TagimgRepository>(
          std::move(*tagimg_repository));

  // libvips 必须在任何转码请求之前初始化；失败时直接退出，避免运行中
  // 才发现 AVIF 编码器不可用。
  if (!tagimg::initialize_image_processing()) {
    std::cerr << "image processing initialization failed" << std::endl;
    return 2;
  }

  tagimg::ImageProcessingConfig image_processing;
  image_processing.quality = config->http.image_quality;
  image_processing.speed = config->http.image_speed;
  image_processing.max_concurrency = config->http.image_max_concurrency;
  image_processing.max_pixels = config->http.image_max_pixels;
  image_processing.timeout_seconds = config->http.image_timeout_seconds;

  // 图片流管理器要活到 Drogon 停止之后，因此声明在 service 之前。
  auto image_streamer = std::make_shared<tagimg::ImageStreamer>(
      entity_repository_shared, tagimg_repository_shared,
      config->http.image_mode, image_processing, work_dir);

  // 扫描任务由 REST 与 GraphQL 共用同一个服务实例。
  auto runtime = std::make_shared<tagimg::ServiceRuntime>(
      entity_repository_shared, tagimg_repository_shared, tagger, embedder,
      work_dir, config->ai.clip_strict);

  auto service = std::make_shared<tagimg::ApiService>(runtime);
  auto graphql_service = std::make_shared<tagimg::GraphQLService>(
      runtime, tagimg_repository_shared, embedder);

  drogon::app().registerHandler(
      "/api/v1",
      [service](
          const drogon::HttpRequestPtr &request,
          std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        service->handle_rpc(request, std::move(callback));
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/v1/schema",
      [service](
          const drogon::HttpRequestPtr &request,
          std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        service->handle_schema(request, std::move(callback));
      },
      {drogon::Get});

  drogon::app().registerHandler(
      "/graphql",
      [graphql_service](
          const drogon::HttpRequestPtr &request,
          std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        graphql_service->handle(request, std::move(callback));
      },
      {drogon::Post});

  drogon::app().registerHandler(
      "/api/v1/image/{string imageId}",
      [image_streamer](
          const drogon::HttpRequestPtr &request,
          std::function<void(const drogon::HttpResponsePtr &)> &&callback,
          std::string image_id) {
        tagimg::ImageStreamOptions options;
        options.image_id = std::move(image_id);
        options.storage_id = request->getParameter("storageId");
        options.size = request->getParameter("size");

        const std::string &download = request->getParameter("download");
        options.download = download == "1" || download == "true";

        std::string request_id = request->getParameter("requestId");
        if (request_id.empty()) {
          request_id = request->getHeader("x-request-id");
        }
        if (!request_id.empty()) {
          options.request_id = std::move(request_id);
        }
        const std::string &if_none_match = request->getHeader("if-none-match");
        if (!if_none_match.empty()) {
          options.if_none_match = if_none_match;
        }

        image_streamer->handle(std::move(options), std::move(callback));
      },
      {drogon::Get});

  // 前端页面与 API 同源托管；目录默认是配置目录下的 web/，可用
  // TAGIMG_WEB_DIR 覆盖（相对路径仍按配置目录解析）。
  const std::filesystem::path web_dir =
      resolve_path(config_dir, env_or("TAGIMG_WEB_DIR", "web"));
  std::error_code web_error;
  if (std::filesystem::is_directory(web_dir, web_error) && !web_error) {
    drogon::app().setDocumentRoot(web_dir.string());
    std::cout << "web root " << web_dir << std::endl;
  } else {
    std::cerr << "web directory not found: " << web_dir << std::endl;
  }

  drogon::app().setClientMaxBodySize(1024 * 1024);
  drogon::app().addListener(config->http.host, config->http.port);

  std::cout << "tagimg listening on http://" << config->http.host << ':'
            << config->http.port << std::endl;

  try {
    drogon::app().run();
  } catch (const std::exception &exception) {
    std::cerr << "server error: " << exception.what() << '\n';
    service->shutdown();
    image_streamer->shutdown();
    return 1;
  }

  service->shutdown();
  image_streamer->shutdown();
  return 0;
}
