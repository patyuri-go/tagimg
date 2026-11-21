#pragma once

#include <optional>
#include <string_view>

namespace tagimg {

// GET /api/v1/image/{imageId}?size=... 支持的输出尺寸。
//
// 图片缓存层也用这组值作为质量分级：尺寸越小质量越低、缓存优先级越高。
enum class ImageSizePreset {
  Mini,
  Thumb,
  Small,
  Regular,
  Original,
};

[[nodiscard]] std::optional<ImageSizePreset>
parse_image_preset(std::string_view value) noexcept;

[[nodiscard]] std::string_view to_string(ImageSizePreset preset) noexcept;

} // namespace tagimg
