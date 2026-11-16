#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "common/result.hpp"

namespace tagimg {

// 拉起 Python 推理服务所需的参数。
struct SidecarConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = 8001;
  std::filesystem::path python;
  std::filesystem::path python_path;
  std::filesystem::path model_dir;
  std::string module = "tagimg_ai.server";
  std::uint32_t threads = 2;
  std::chrono::seconds startup_timeout{90};

  // Chinese-CLIP 向量模型；clip_preload 为 true 时会在 lifespan 阶段加载权重。
  std::filesystem::path clip_model_dir;
  std::uint32_t clip_threads = 2;
  bool clip_preload = true;
  // 预加载 CLIP 时，启动等待上限要覆盖读入权重的耗时。
  std::chrono::seconds clip_startup_timeout{180};
};

enum class SidecarError {
  InvalidConfig,
  SpawnFailed,
  StartupTimeout,
};

[[nodiscard]] std::string_view to_string(SidecarError error) noexcept;

// 管理 Python 推理子进程。
//
// start() 先探测 /health：如果已有可用的 sidecar 就直接复用（managed() 为
// false），否则 fork + exec 拉起 python -m <module>，等 /health 通过后返回。
// 析构时向子进程组发送 SIGTERM，超时后 SIGKILL，并回收子进程，保证
// tagimg_server 退出时 Python 不会残留。
class SidecarProcess {
public:
  [[nodiscard]] static Result<std::unique_ptr<SidecarProcess>, SidecarError>
  start(const SidecarConfig &config);

  ~SidecarProcess();

  SidecarProcess(const SidecarProcess &) = delete;
  SidecarProcess &operator=(const SidecarProcess &) = delete;
  SidecarProcess(SidecarProcess &&) = delete;
  SidecarProcess &operator=(SidecarProcess &&) = delete;

  // 子进程是否由本对象拉起；复用的外部 sidecar 为 false。
  [[nodiscard]] bool managed() const noexcept;

  // 可直接交给 WdTagger 的 base URL。
  [[nodiscard]] std::string endpoint() const;

  // 停止并回收子进程；可以重复调用。
  void stop() noexcept;

private:
  SidecarProcess();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace tagimg
