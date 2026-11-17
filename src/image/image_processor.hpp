#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/result.hpp"

namespace tagimg {

// GET /api/v1/image/{imageId}?size=... 支持的输出尺寸。
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

// AVIF 编码参数。max_concurrency 只限制同时进行的转码任务数，
// libvips 内部固定为单线程，避免 aom 把整机 CPU 打满。
struct ImageProcessingConfig {
  std::uint32_t quality = 65;
  std::uint32_t speed = 6;
  std::uint32_t max_concurrency = 2;
  // 解码后允许的最大像素数；超过时在真正解码前拒绝。
  std::uint64_t max_pixels = 100'000'000;
  // 单个转码任务从开始处理到结束的时间上限。
  std::uint32_t timeout_seconds = 30;
};

[[nodiscard]] int preset_dimension(ImageSizePreset preset) noexcept;

// heifsave 的选项串，例如 ".avif[Q=65,effort=6]"。
[[nodiscard]] std::string
avif_save_options(const ImageProcessingConfig &config);

struct EncodedImage {
  std::vector<std::byte> bytes;
};

enum class ImageProcessingError {
  InitializationFailed,
  InvalidPreset,
  DecodeFailed,
  EncodeFailed,
};

[[nodiscard]] std::string_view
to_string(ImageProcessingError error) noexcept;

// 初始化 libvips；幂等，并且可以从多个线程同时调用。
[[nodiscard]] bool initialize_image_processing();

// 把任意 libvips 能解码的图片缩放后编码为 AVIF。
//
// 缩放语义是「等比缩到 max(width,height) 的框内，不放大」；preset 不能是
// Original。调用方负责保证 source 在调用期间有效。
[[nodiscard]] Result<EncodedImage, ImageProcessingError>
encode_avif(const std::vector<std::byte> &source, ImageSizePreset preset,
            const ImageProcessingConfig &config);

} // namespace tagimg
