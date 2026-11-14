#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

#include "common/result.hpp"

namespace tagimg {

enum class TempFileError {
  CreateFailed,
  WriteFailed,
};

[[nodiscard]] std::string_view to_string(TempFileError error) noexcept;

// 临时文件 RAII 对象。
//
// create() 在指定目录下以 O_EXCL 方式创建唯一文件；对象析构时关闭并
// 删除文件。只允许移动，不允许拷贝，避免两个对象管理同一个文件。
class TempFile {
public:
  static Result<TempFile, TempFileError>
  create(const std::filesystem::path &directory,
         std::string_view extension = ".part");

  ~TempFile();

  TempFile(const TempFile &) = delete;
  TempFile &operator=(const TempFile &) = delete;
  TempFile(TempFile &&other) noexcept;
  TempFile &operator=(TempFile &&other) noexcept;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

  [[nodiscard]] std::uint64_t size() const noexcept {
    return size_;
  }

  [[nodiscard]] Result<void, TempFileError>
  write(std::span<const std::byte> bytes);

  [[nodiscard]] Result<void, TempFileError> close();

private:
  TempFile(std::filesystem::path path, int fd);

  void reset() noexcept;
  void take_from(TempFile &other) noexcept;

  std::filesystem::path path_;
  int fd_ = -1;
  std::uint64_t size_ = 0;
};

} // namespace tagimg
