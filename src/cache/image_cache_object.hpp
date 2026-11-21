#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>

#include "cache/image_cache_key.hpp"
#include "common/result.hpp"

namespace tagimg {

enum class ImageCacheError {
  NotEnabled,
  OpenFailed,
  SchemaFailed,
  QueryFailed,
  IoFailed,
  InvalidKey,
};

[[nodiscard]] std::string_view to_string(ImageCacheError error) noexcept;

enum class ImageCacheCommit {
  Committed,
  SkippedOverLimit,
};

namespace cache_detail {
struct ObjectImpl;
} // namespace cache_detail

// 「已入库图片缓存对象」：move-only RAII 句柄。
//
// find() 命中时返回可读对象；create() 返回可写对象。写对象析构时若没有
// commit()，临时文件会被删除。
class StoredImageCacheObject {
public:
  ~StoredImageCacheObject();

  StoredImageCacheObject(const StoredImageCacheObject &) = delete;
  StoredImageCacheObject &operator=(const StoredImageCacheObject &) = delete;
  StoredImageCacheObject(StoredImageCacheObject &&other) noexcept;
  StoredImageCacheObject &operator=(StoredImageCacheObject &&other) noexcept;

  [[nodiscard]] bool readable() const noexcept;
  [[nodiscard]] bool writable() const noexcept;

  // 仅读对象可用；误用时抛 std::logic_error。
  std::istream &read();
  // 仅未提交的写对象可用；误用时抛 std::logic_error。
  std::ostream &write();

  // flush 写流但不关闭；之后可安全读取 temporary_path()。仅未提交写对象可用。
  [[nodiscard]] Result<void, ImageCacheError> flush();

  // 写完调用 commit()。返回 Committed 或 SkippedOverLimit。
  [[nodiscard]] Result<ImageCacheCommit, ImageCacheError> commit();

  // commit() 成功后调用：重新登记 reader 并打开正式文件，防止解码期间被淘汰。
  [[nodiscard]] Result<void, ImageCacheError> pin();

  // 删除当前条目（DB 行 + 正式文件）。有其它 reader/writer 时返回 false。
  bool remove();

  [[nodiscard]] const ImageCacheKey &key() const noexcept;
  [[nodiscard]] const std::string &content_type() const noexcept;
  [[nodiscard]] std::uint64_t size() const noexcept;
  // 正式文件的绝对路径（写对象在 commit 前也可用于诊断）。
  [[nodiscard]] const std::filesystem::path &file_path() const noexcept;
  // commit() 前的临时文件绝对路径。
  [[nodiscard]] const std::filesystem::path &temporary_path() const noexcept;

  // 在 commit() 前设置实际 Content-Type（源缓存用于记录真实 MIME）。
  void set_content_type(std::string_view content_type) noexcept;

private:
  friend class ImageCacheStore;

  explicit StoredImageCacheObject(
      std::unique_ptr<cache_detail::ObjectImpl> impl);

  std::unique_ptr<cache_detail::ObjectImpl> impl_;
};

} // namespace tagimg
