#include "ai/sidecar_process.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace tagimg {
namespace {

// 单次健康探测的超时，避免 sidecar 没起来时把启动流程卡住。
constexpr int kProbeTimeoutMs = 300;
constexpr auto kProbeInterval = std::chrono::milliseconds(200);
constexpr auto kStopTimeout = std::chrono::seconds(10);

bool probe_health(const std::string &host, std::uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo *addresses = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses) != 0) {
    return false;
  }

  const std::string request = "GET /health HTTP/1.1\r\nHost: " + host + ":" +
                              service + "\r\nConnection: close\r\n\r\n";
  bool healthy = false;

  for (addrinfo *entry = addresses; entry != nullptr && !healthy;
       entry = entry->ai_next) {
    const int fd = ::socket(entry->ai_family, entry->ai_socktype | SOCK_CLOEXEC,
                            entry->ai_protocol);
    if (fd < 0) {
      continue;
    }

    // 非阻塞 connect + poll，单次探测最多等 kProbeTimeoutMs。
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int connected = ::connect(fd, entry->ai_addr, entry->ai_addrlen);
    if (connected != 0 && errno == EINPROGRESS) {
      pollfd wait_for_connect{fd, POLLOUT, 0};
      if (::poll(&wait_for_connect, 1, kProbeTimeoutMs) == 1) {
        int socket_error = 0;
        socklen_t length = sizeof(socket_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) ==
                0 &&
            socket_error == 0) {
          connected = 0;
        }
      }
    }

    if (connected == 0) {
      ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
      pollfd wait_for_reply{fd, POLLIN, 0};
      if (::poll(&wait_for_reply, 1, kProbeTimeoutMs) == 1) {
        char buffer[64] = {};
        const ssize_t received = ::recv(fd, buffer, sizeof(buffer) - 1, 0);
        if (received > 0) {
          const std::string_view response(buffer,
                                          static_cast<std::size_t>(received));
          healthy = response.rfind("HTTP/1.", 0) == 0 &&
                    response.find(" 200") != std::string_view::npos;
        }
      }
    }
    ::close(fd);
  }

  ::freeaddrinfo(addresses);
  return healthy;
}

// 由父进程调用；返回子进程 pid，失败返回 -1。
pid_t spawn_sidecar(const SidecarConfig &config) {
  const pid_t pid = ::fork();
  if (pid != 0) {
    return pid;
  }

  // 子进程：独立进程组，父进程退出时收到 SIGTERM。
  ::setpgid(0, 0);
  ::prctl(PR_SET_PDEATHSIG, SIGTERM);

  ::setenv("PYTHONPATH", config.python_path.c_str(), 1);
  ::setenv("PYTHONUNBUFFERED", "1", 1);
  ::setenv("TAGIMG_MODEL_DIR", config.model_dir.c_str(), 1);
  ::setenv("TAGIMG_TAGGER_THREADS", std::to_string(config.threads).c_str(), 1);
  if (!config.clip_model_dir.empty()) {
    ::setenv("TAGIMG_CLIP_MODEL_DIR", config.clip_model_dir.c_str(), 1);
  }
  ::setenv("TAGIMG_CLIP_THREADS", std::to_string(config.clip_threads).c_str(), 1);
  ::setenv("TAGIMG_CLIP_PRELOAD", config.clip_preload ? "1" : "0", 1);
  ::setenv("TAGIMG_AI_HOST", config.host.c_str(), 1);
  ::setenv("TAGIMG_AI_PORT", std::to_string(config.port).c_str(), 1);

  const std::string python = config.python.string();
  const std::string module = config.module;
  std::vector<char *> arguments;
  arguments.push_back(const_cast<char *>(python.c_str()));
  arguments.push_back(const_cast<char *>("-m"));
  arguments.push_back(const_cast<char *>(module.c_str()));
  arguments.push_back(nullptr);

  ::execv(python.c_str(), arguments.data());
  ::_exit(127);
}

} // namespace

struct SidecarProcess::Impl {
  SidecarConfig config;
  std::string connect_host;
  pid_t child = -1;
  bool managed = false;
};

std::string_view to_string(SidecarError error) noexcept {
  switch (error) {
  case SidecarError::InvalidConfig:
    return "InvalidConfig";
  case SidecarError::SpawnFailed:
    return "SpawnFailed";
  case SidecarError::StartupTimeout:
    return "StartupTimeout";
  }
  return "Unknown";
}

SidecarProcess::SidecarProcess() : impl_(std::make_unique<Impl>()) {
}

SidecarProcess::~SidecarProcess() {
  stop();
}

bool SidecarProcess::managed() const noexcept {
  return impl_->managed && impl_->child > 0;
}

std::string SidecarProcess::endpoint() const {
  return "http://" + impl_->connect_host + ":" +
         std::to_string(impl_->config.port);
}

Result<std::unique_ptr<SidecarProcess>, SidecarError>
SidecarProcess::start(const SidecarConfig &config) {
  if (config.host.empty() || config.port == 0 || config.module.empty() ||
      config.python.empty() || config.python_path.empty() ||
      config.model_dir.empty() || config.threads == 0 ||
      config.startup_timeout.count() <= 0) {
    return SidecarError::InvalidConfig;
  }

  std::error_code error;
  if (!std::filesystem::is_regular_file(config.python, error) || error) {
    return SidecarError::InvalidConfig;
  }
  if (!std::filesystem::is_directory(config.model_dir, error) || error) {
    return SidecarError::InvalidConfig;
  }

  auto sidecar = std::unique_ptr<SidecarProcess>(new SidecarProcess());
  sidecar->impl_->config = config;
  sidecar->impl_->connect_host =
      (config.host == "0.0.0.0" || config.host == "::") ? "127.0.0.1"
                                                        : config.host;

  // 已经有可用的 sidecar（例如手动起的）就直接复用，退出时不去杀它。
  if (probe_health(sidecar->impl_->connect_host, config.port)) {
    sidecar->impl_->managed = false;
    return sidecar;
  }

  const pid_t pid = spawn_sidecar(config);
  if (pid < 0) {
    return SidecarError::SpawnFailed;
  }
  sidecar->impl_->child = pid;
  sidecar->impl_->managed = true;

  // CLIP 预加载发生在 lifespan 内，/health 要等权重读完才会返回 200，
  // 因此启动等待上限要取两者较大值。
  auto startup_timeout = config.startup_timeout;
  if (config.clip_preload && config.clip_startup_timeout > startup_timeout) {
    startup_timeout = config.clip_startup_timeout;
  }

  const auto deadline = std::chrono::steady_clock::now() + startup_timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (probe_health(sidecar->impl_->connect_host, config.port)) {
      return sidecar;
    }

    int status = 0;
    if (::waitpid(pid, &status, WNOHANG) == pid) {
      sidecar->impl_->child = -1;
      return SidecarError::SpawnFailed;
    }
    std::this_thread::sleep_for(kProbeInterval);
  }

  sidecar->stop();
  return SidecarError::StartupTimeout;
}

void SidecarProcess::stop() noexcept {
  if (!impl_ || impl_->child <= 0) {
    return;
  }

  const pid_t pid = impl_->child;
  // 子进程自成一个进程组，整组杀掉，避免 uvicorn 的 reload/worker 残留。
  ::kill(-pid, SIGTERM);
  ::kill(pid, SIGTERM);

  const auto deadline = std::chrono::steady_clock::now() + kStopTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    int status = 0;
    if (::waitpid(pid, &status, WNOHANG) == pid) {
      impl_->child = -1;
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  ::kill(-pid, SIGKILL);
  ::kill(pid, SIGKILL);
  ::waitpid(pid, nullptr, 0);
  impl_->child = -1;
}

} // namespace tagimg
