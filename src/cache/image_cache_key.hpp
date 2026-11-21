#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "common/image_size.hpp"

namespace tagimg {

// 索引表 cache_type 列的取值。将来可以有 unstored_image_cache 等。
inline constexpr std::string_view kImageCacheType = "stored_image_cache";

// canonical key 的格式版本。编码参数语义变化时 +1，一次性作废旧缓存。
inline constexpr std::string_view kImageCacheFormatVersion = "v1";

// 参与缓存 key 的压缩参数。所有字段都会写进 canonical 文本，缺省值也写，
// 保证同一语义只有一种 key。
struct ImageCacheParams {
  ImageSizePreset preset = ImageSizePreset::Original;
  std::uint32_t quality = 0;
  std::uint32_t speed = 0;
  std::uint32_t animated_quality = 0;
  std::uint32_t animated_speed = 0;
  std::uint32_t animated_max_frames = 0;
  std::uint64_t animated_max_total_pixels = 0;
  std::uint64_t max_output_bytes = 0;
  // 文件扩展名，不含点。空串按 "bin" 处理。
  std::string extension = "bin";
  // 回放时使用的 Content-Type；为空时按扩展名推断。
  std::string content_type;
};

// 使用者先构造的「已入库图片缓存的结构体」：图片 id + 压缩参数。
struct ImageCacheKey {
  std::string image_id;
  ImageCacheParams params;
};

// 质量分级即尺寸分级：质量越高优先级越低，数值越小越先淘汰。
[[nodiscard]] int preset_priority(ImageSizePreset preset) noexcept;

// 小写化扩展名；空串返回 "bin"。
[[nodiscard]] std::string normalize_extension(std::string_view extension);

// 校验 image_id 与非空扩展名。扩展名会先小写化。
[[nodiscard]] bool is_valid_cache_key(const ImageCacheKey &key) noexcept;

// 固定顺序拼接全部字段。调用前应保证 key 合法。
[[nodiscard]] std::string canonical_cache_key(const ImageCacheKey &key);

// SHA-256(canonical) 的前 32 个十六进制字符（128 位）。
[[nodiscard]] std::string cache_key_hash(std::string_view canonical);

// content_type 为空时按扩展名推断。
[[nodiscard]] std::string_view
infer_content_type(std::string_view extension) noexcept;

} // namespace tagimg
