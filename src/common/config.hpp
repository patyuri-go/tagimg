#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "common/result.hpp"

namespace tagimg {

struct DatabaseConfig {
  std::string host;
  std::uint16_t port = 5432;
  std::string user;
  std::string password;
  std::string image_dbname;
  std::string entity_dbname;
};

// 图片流接口的服务模式。
enum class ImageStreamMode {
  // 服务端读取上游字节再转发给客户端，即现有行为。
  Proxy,
  // 服务端先跟随上游重定向拿到直链，再返回 302 让客户端直连。
  Redirect,
};

// HTTP 服务的监听配置。
struct HttpConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = 8080;
  // 缺省为本机代理，保证旧配置文件行为不变。
  ImageStreamMode image_mode = ImageStreamMode::Proxy;
  // AVIF 编码参数；具体含义见 ImageProcessingConfig。
  std::uint32_t image_quality = 65;
  std::uint32_t image_speed = 6;
  // 同时进行的转码任务数上限；libvips 内部固定单线程。
  std::uint32_t image_max_concurrency = 2;
  // 解码后的像素上限；超过时在真正解码前返回 413。
  std::uint64_t image_max_pixels = 100'000'000;
  // 单个图片转码任务的超时时间，从拿到 worker 开始计算。
  std::uint32_t image_timeout_seconds = 30;
};

// Python 推理服务（sidecar）的启动参数。
//
// 相对路径按配置文件所在目录解析。`module` 是 python -m 的模块名。
struct AiConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = 8001;
  std::filesystem::path python = ".venv/bin/python";
  // 传给子进程的 PYTHONPATH。
  std::filesystem::path python_path = "python";
  std::filesystem::path model_dir = "models/wd-swinv2-tagger-v3";
  std::string module = "tagimg_ai.server";
  std::uint32_t threads = 2;
  std::uint32_t startup_timeout_seconds = 90;

  // Chinese-CLIP 图像/文本向量模型（768 维）。
  std::filesystem::path clip_model_dir =
      "models/chinese-clip-vit-large-patch14";
  std::uint32_t clip_threads = 2;
  // 启动时就加载 CLIP 权重，避免首个扫描任务承担加载耗时。
  bool clip_preload = true;
  // 向量生成失败时阻塞入库（整单回滚），保证“有图必有向量”。
  bool clip_strict = true;
  // 单次向量请求超时；CPU 上 ViT-L/14 明显慢于 tagger。
  std::uint32_t clip_timeout_seconds = 60;
  // 等待 sidecar 报告 clip.loaded 的上限，加载 1.6GB 权重需要时间。
  std::uint32_t clip_startup_timeout_seconds = 180;
};

struct AppConfig {
  DatabaseConfig database;
  // http 段可以省略，省略时使用上面的默认监听地址。
  HttpConfig http;
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

// 加载 JSON 配置文件。文件中至少要有 "database" 对象，
// 包含 host、port、user、password、image_dbname、entity_dbname 字段。
[[nodiscard]] Result<AppConfig, ConfigError>
load_config(const std::filesystem::path &path);

} // namespace tagimg
