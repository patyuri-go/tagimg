#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "common/result.hpp"
#include "common/storage.hpp"

namespace tagimg {

struct DatabaseConfig {
  std::string host;
  std::uint16_t port = 5432;
  std::string user;
  std::string password;
  std::string image_dbname;
};

// 图片流接口的服务模式。
enum class ImageStreamMode {
  // 服务端读取上游字节再转发给客户端。
  Proxy,
  // 直接返回 302，让客户端访问存储服务的内容地址。
  Redirect,
};

// HTTP 服务的监听配置。
struct HttpConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = 8080;
  // 缺省为服务端代理，保证旧配置文件行为不变。
  ImageStreamMode image_mode = ImageStreamMode::Proxy;
  // AVIF 编码参数：quality 为 1-100，speed 为 0-9 且越大越快
  // （0 最慢、9 最快），与 imageAnimatedSpeed 语义一致。
  std::uint32_t image_quality = 65;
  std::uint32_t image_speed = 6;
  // 同时进行的转码任务数上限；libvips 内部固定单线程。
  std::uint32_t image_max_concurrency = 2;
  // 解码后的像素上限；超过时在真正解码前返回 413。
  std::uint64_t image_max_pixels = 100'000'000;
  // 单个图片转码任务的超时时间，从拿到 worker 开始计算。
  std::uint32_t image_timeout_seconds = 30;
  // 动图（GIF / 动图 WebP）转码参数。
  // quality/speed 为空时分别继承 image_quality / image_speed。
  std::optional<std::uint32_t> image_animated_quality;
  std::optional<std::uint32_t> image_animated_speed;
  std::uint32_t image_animated_max_frames = 300;
  std::uint64_t image_animated_max_total_pixels = 300'000'000;
};

// 图片缓存层配置。cache 段缺失时 enabled 保持 false。
struct ImageCacheConfig {
  bool enabled = false;
  // 相对路径按配置文件所在目录解析。
  std::filesystem::path directory = "tmp/images";
  // 0 表示不限制容量。
  std::uint64_t max_bytes = 2ULL * 1024 * 1024 * 1024;
  // open 时是否逐行核对文件并清理孤儿文件。
  bool reconcile_on_open = true;
};

// Python 推理服务（sidecar）的启动参数。
//
// 相对路径按配置文件所在目录解析。`module` 是 python -m 的模块名。
struct AiConfig {
  // true：启动/复用本地 Python sidecar（旧行为）。
  // false：不启动 Python，只使用 host:port 指向的远程 AI 服务。
  bool local = true;
  std::string host = "127.0.0.1";
  std::uint16_t port = 8001;
  // 启动探测与每个任务预检 /health 的超时时间。
  std::uint32_t health_timeout_ms = 1000;
  std::filesystem::path python = ".venv/bin/python";
  // 传给子进程的 PYTHONPATH。
  std::filesystem::path python_path = "python";
  std::filesystem::path model_dir = "models/wd-swinv2-tagger-v3";
  std::string module = "tagimg_ai.server";
  std::uint32_t threads = 2;
  std::uint32_t startup_timeout_seconds = 90;
  // wd14 general / character 标签的统一置信度阈值。
  //
  // 配置文件必须显式提供；加载器不会使用这里的默认值兜底。
  float wd14_threshold = 0.85F;

  // Chinese-CLIP 图像/文本向量模型（768 维）。
  std::filesystem::path clip_model_dir =
      "models/chinese-clip-vit-large-patch14";
  std::uint32_t clip_threads = 2;
  // 启动时就加载 CLIP 权重，避免首个扫描任务承担加载耗时。
  bool clip_preload = true;
  // 已弃用：CLIP 现在始终是入库前置条件，该字段只为兼容旧配置保留。
  bool clip_strict = true;
  // 配置文件里是否显式写了 clip_strict，用于启动时打印弃用警告。
  bool clip_strict_configured = false;
  // 单次向量请求超时；CPU 上 ViT-L/14 明显慢于 tagger。
  std::uint32_t clip_timeout_seconds = 60;
  // 等待 sidecar 报告 clip.loaded 的上限，加载 1.6GB 权重需要时间。
  std::uint32_t clip_startup_timeout_seconds = 180;
};

struct AppConfig {
  DatabaseConfig database;
  // 外部存储服务地址；图片内容与扫描下载都走这里。
  StorageConfig storage;
  // http 段可以省略，省略时使用上面的默认监听地址。
  HttpConfig http;
  // cache 段可以省略，省略时缓存层禁用。
  ImageCacheConfig cache;
  // ai 段可以省略，省略时使用上面的默认参数。
  AiConfig ai;
};

enum class ConfigError {
  FileNotFound,
  ReadError,
  ParseError,
  MissingField,
  InvalidField,
};

[[nodiscard]] std::string_view to_string(ConfigError error) noexcept;

// 加载 JSON 配置文件。文件中至少要有 "database" 与 "storage" 对象。
// database 包含 host、port、user、password、image_dbname 字段；
// storage 包含 scheme（可选，缺省 http）、host、port 字段。
[[nodiscard]] Result<AppConfig, ConfigError>
load_config(const std::filesystem::path &path);

} // namespace tagimg
