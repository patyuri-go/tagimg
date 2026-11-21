#pragma once

#include <filesystem>
#include <memory>
#include <optional>

#include "cache/image_cache_object.hpp"
#include "common/config.hpp"
#include "common/result.hpp"

namespace tagimg {

// 图片缓存仓库：管理索引库、缓存目录、引用计数与淘汰。
class ImageCacheStore {
public:
  // base_dir 用于解析相对的 config.directory；正常由 load_config
  // 解析成绝对路径。
  static Result<ImageCacheStore, ImageCacheError>
  open(const ImageCacheConfig &config, const std::filesystem::path &base_dir);

  ~ImageCacheStore();

  ImageCacheStore(const ImageCacheStore &) = delete;
  ImageCacheStore &operator=(const ImageCacheStore &) = delete;
  ImageCacheStore(ImageCacheStore &&other) noexcept;
  ImageCacheStore &operator=(ImageCacheStore &&other) noexcept;

  // 未命中返回空 optional；命中时已经刷新 last_hit 并持有读引用。
  [[nodiscard]] Result<std::optional<StoredImageCacheObject>, ImageCacheError>
  find(const ImageCacheKey &key);

  // 总是返回可写对象；commit() 时才决定是否真正入库。
  [[nodiscard]] Result<StoredImageCacheObject, ImageCacheError>
  create(const ImageCacheKey &key);

private:
  struct Impl;
  explicit ImageCacheStore(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

} // namespace tagimg
