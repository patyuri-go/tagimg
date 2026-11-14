#include "io/temp_file.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <random>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace tagimg {
namespace {

std::atomic<std::uint64_t> g_counter{0};

} // namespace

std::string_view to_string(TempFileError error) noexcept {
  switch (error) {
  case TempFileError::CreateFailed:
    return "CreateFailed";
  case TempFileError::WriteFailed:
    return "WriteFailed";
  }
  return "Unknown";
}

Result<TempFile, TempFileError>
TempFile::create(const std::filesystem::path &directory,
                 std::string_view extension) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error || !std::filesystem::is_directory(directory, error)) {
    return TempFileError::CreateFailed;
  }

  for (int attempt = 0; attempt < 32; ++attempt) {
    const std::uint64_t sequence = g_counter.fetch_add(1);
    std::random_device random;
    const std::string name = "tagimg-" + std::to_string(::getpid()) + "-" +
                             std::to_string(sequence) + "-" +
                             std::to_string(random()) + std::string(extension);
    const std::filesystem::path path = directory / name;

    const int fd =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd >= 0) {
      return TempFile(path, fd);
    }
    if (errno != EEXIST) {
      break;
    }
  }
  return TempFileError::CreateFailed;
}

TempFile::TempFile(std::filesystem::path path, int fd)
    : path_(std::move(path)), fd_(fd) {
}

TempFile::~TempFile() {
  reset();
}

TempFile::TempFile(TempFile &&other) noexcept {
  take_from(other);
}

TempFile &TempFile::operator=(TempFile &&other) noexcept {
  if (this != &other) {
    reset();
    take_from(other);
  }
  return *this;
}

Result<void, TempFileError> TempFile::write(std::span<const std::byte> bytes) {
  if (fd_ < 0) {
    return TempFileError::WriteFailed;
  }

  while (!bytes.empty()) {
    const ssize_t written = ::write(fd_, bytes.data(), bytes.size());
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return TempFileError::WriteFailed;
    }
    bytes = bytes.subspan(static_cast<std::size_t>(written));
    size_ += static_cast<std::uint64_t>(written);
  }
  return Result<void, TempFileError>::ok();
}

Result<void, TempFileError> TempFile::close() {
  if (fd_ < 0) {
    return Result<void, TempFileError>::ok();
  }

  const int result = ::close(fd_);
  fd_ = -1;
  if (result != 0) {
    return TempFileError::WriteFailed;
  }
  return Result<void, TempFileError>::ok();
}

void TempFile::reset() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if (!path_.empty()) {
    std::error_code error;
    std::filesystem::remove(path_, error);
    path_.clear();
  }
  size_ = 0;
}

void TempFile::take_from(TempFile &other) noexcept {
  path_ = std::move(other.path_);
  fd_ = other.fd_;
  size_ = other.size_;
  other.path_.clear();
  other.fd_ = -1;
  other.size_ = 0;
}

} // namespace tagimg
