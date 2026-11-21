#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include <drogon/drogon.h>

#include "ai/clip_embedder.hpp"
#include "ai/health.hpp"
#include "ai/sidecar_process.hpp"
#include "ai/tagger.hpp"
#include "cache/image_cache_store.hpp"
#include "common/config.hpp"
#include "db/tagimg_repository.hpp"
#include "graphql/graphql_service.hpp"
#include "http/api_service.hpp"
#include "http/image_stream.hpp"
#include "http/request_context.hpp"
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

// 远程模式下只根据 host/port 拼 endpoint，不触碰任何本地路径。
std::string remote_ai_endpoint(const tagimg::AiConfig &ai) {
  std::string host = ai.host;
  if (host == "0.0.0.0" || host == "::" || host == "[::]") {
    host = "127.0.0.1";
  }
  return "http://" + host + ":" + std::to_string(ai.port);
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

  tagimg::TagimgDbConfig tagimg_db;
  tagimg_db.host = config->database.host;
  tagimg_db.port = config->database.port;
  tagimg_db.user = config->database.user;
  tagimg_db.password = config->database.password;
  tagimg_db.dbname = config->database.image_dbname;

  auto tagimg_repository = tagimg::TagimgRepository::connect(tagimg_db, 8);
  if (!tagimg_repository) {
    std::cerr << "tagimg database error: "
              << tagimg::to_string(tagimg_repository.error()) << '\n';
    return 2;
  }

  // sidecar 必须声明在 service 之前：析构顺序相反，先停 worker 再杀 Python。
  // ai.local=false 时完全不创建 SidecarProcess，也不会校验本地路径。
  std::unique_ptr<tagimg::SidecarProcess> sidecar;
  std::string endpoint;
  if (config->ai.local) {
    auto started = tagimg::SidecarProcess::start(
        make_sidecar_config(config->ai, config_dir));
    if (!started) {
      std::cerr << "sidecar error: " << tagimg::to_string(started.error())
                << '\n';
      return 2;
    }
    sidecar = std::move(*started);
    endpoint = sidecar->endpoint();
    std::cout << "sidecar " << endpoint << ' '
              << (sidecar->managed() ? "spawned" : "reused") << std::endl;
  } else {
    endpoint = remote_ai_endpoint(config->ai);
    std::cout << "ai remote " << endpoint << std::endl;
  }

  if (config->ai.clip_strict_configured) {
    std::cerr
        << "warning: ai.clip_strict is deprecated and ignored; CLIP is always "
           "required"
        << std::endl;
  }

  // 远程模式下这次探测失败只打印警告，服务照常启动；具体任务会在
  // 下载前再做一次预检并返回 AI_UNAVAILABLE。
  const tagimg::AiHealthClient ai_health(
      endpoint, std::chrono::milliseconds(config->ai.health_timeout_ms));
  const auto health = ai_health.check();
  if (health && health->ready()) {
    std::cout << "ai models ready" << std::endl;
  } else if (health) {
    std::cerr << "warning: ai models not ready (tagger="
              << (health->tagger_loaded ? "loaded" : "unavailable")
              << ", clip=" << (health->clip_loaded ? "loaded" : "unavailable")
              << ')' << std::endl;
  } else {
    std::cerr << "warning: ai health check failed: "
              << tagimg::to_string(health.error()) << std::endl;
  }

  auto tagger = std::make_shared<tagimg::WdTagger>(
      endpoint, config->ai.wd14_threshold);
  // CLIP 与 tagger 共用同一个 sidecar；CPU 上 ViT-L/14 明显更慢，超时单独配置。
  auto embedder = std::make_shared<tagimg::ClipEmbedder>(
      endpoint, std::chrono::seconds(config->ai.clip_timeout_seconds));
  const std::filesystem::path work_dir(
      env_or("TAGIMG_WORK_DIR",
             (std::filesystem::temp_directory_path() / "tagimg").string()));

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
  image_processing.animated_quality =
      config->http.image_animated_quality.value_or(
          config->http.image_quality);
  image_processing.animated_speed =
      config->http.image_animated_speed.value_or(config->http.image_speed);
  image_processing.animated_max_frames =
      config->http.image_animated_max_frames;
  image_processing.animated_max_total_pixels =
      config->http.image_animated_max_total_pixels;

  // 缓存启用但打不开时直接退出，避免运行期静默失去缓存。
  std::shared_ptr<tagimg::ImageCacheStore> image_cache;
  {
    auto opened = tagimg::ImageCacheStore::open(config->cache, config_dir);
    if (!opened) {
      std::cerr << "image cache open failed: "
                << tagimg::to_string(opened.error()) << std::endl;
      if (config->cache.enabled) {
        return 2;
      }
    } else {
      image_cache =
          std::make_shared<tagimg::ImageCacheStore>(std::move(*opened));
      if (!image_cache->enabled()) {
        std::cout << "image cache disabled" << std::endl;
      }
    }
  }

  // 图片流管理器要活到 Drogon 停止之后，因此声明在 service 之前。
  auto connection_registry = std::make_shared<tagimg::ConnectionRegistry>();
  auto image_streamer = std::make_shared<tagimg::ImageStreamer>(
      tagimg_repository_shared, config->storage, config->http.image_mode,
      image_processing, work_dir, connection_registry, image_cache);

  // 扫描任务由 REST 与 GraphQL 共用同一个服务实例。
  auto runtime = std::make_shared<tagimg::ServiceRuntime>(
      config->storage, tagimg_repository_shared, tagger, embedder, work_dir,
      ai_health, image_cache, image_processing);

  auto service = std::make_shared<tagimg::ApiService>(runtime);
  auto graphql_service = std::make_shared<tagimg::GraphQLService>(
      runtime, tagimg_repository_shared, embedder, connection_registry);

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
        options.size = request->getParameter("size");
        options.context =
            std::make_shared<tagimg::RequestContext>(request->getConnectionPtr());

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
  // 连接断开时取消该连接上仍在排队/执行中的请求。
  drogon::app().setConnectionCallback(
      [connection_registry](const trantor::TcpConnectionPtr &connection) {
        if (!connection->connected()) {
          connection_registry->cancel_connection(connection);
        }
      });
  drogon::app().addListener(config->http.host, config->http.port);

  std::cout << "tagimg listening on http://" << config->http.host << ':'
            << config->http.port << std::endl;

  try {
    drogon::app().run();
  } catch (const std::exception &exception) {
    std::cerr << "server error: " << exception.what() << '\n';
    connection_registry->cancel_all(tagimg::CancelReason::Shutdown);
    graphql_service->shutdown();
    service->shutdown();
    image_streamer->shutdown();
    return 1;
  }

  connection_registry->cancel_all(tagimg::CancelReason::Shutdown);
  graphql_service->shutdown();
  service->shutdown();
  image_streamer->shutdown();
  return 0;
}
