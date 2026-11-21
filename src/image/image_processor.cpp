#include "image/image_processor.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include <vips/vips.h>

namespace tagimg {
namespace {

constexpr int kMiniDimension = 48;
constexpr int kThumbDimension = 250;
constexpr int kSmallDimension = 540;
constexpr int kRegularDimension = 1200;

std::once_flag vips_once;
bool vips_ready = false;

void log_vips_error(std::string_view context) {
  const char *message = vips_error_buffer();
  std::cerr << "image processor: " << context;
  if (message != nullptr && *message != '\0') {
    std::cerr << ": " << message;
  }
  std::cerr << '\n';
  vips_error_clear();
}

void initialize_vips() {
  if (vips_init("tagimg") != 0) {
    log_vips_error("vips_init failed");
    return;
  }
  // 外层已经用 max_concurrency 限制并发；每个操作再开多线程会叠加。
  vips_concurrency_set(1);
  // 不缓存解码后的图像，转码结果由客户端缓存。
  vips_cache_set_max(0);
  vips_ready = true;
}

} // namespace

int preset_dimension(ImageSizePreset preset) noexcept {
  switch (preset) {
  case ImageSizePreset::Mini:
    return kMiniDimension;
  case ImageSizePreset::Thumb:
    return kThumbDimension;
  case ImageSizePreset::Small:
    return kSmallDimension;
  case ImageSizePreset::Regular:
    return kRegularDimension;
  case ImageSizePreset::Original:
    return 0;
  }
  return 0;
}

// imageSpeed 采用“越大越快”的语义（0 最慢、9 最快）。libvips heifsave 的
// effort 正好相反（0 最快、9 最慢），这里换算后再写入。
std::string avif_save_options(const ImageProcessingConfig &config) {
  const std::uint32_t speed = std::min<std::uint32_t>(config.speed, 9);
  return ".avif[Q=" + std::to_string(config.quality) +
         ",effort=" + std::to_string(9 - speed) + "]";
}

std::string_view to_string(ImageProcessingError error) noexcept {
  switch (error) {
  case ImageProcessingError::InitializationFailed:
    return "InitializationFailed";
  case ImageProcessingError::InvalidPreset:
    return "InvalidPreset";
  case ImageProcessingError::DecodeFailed:
    return "DecodeFailed";
  case ImageProcessingError::EncodeFailed:
    return "EncodeFailed";
  }
  return "Unknown";
}

bool initialize_image_processing() {
  std::call_once(vips_once, initialize_vips);
  return vips_ready;
}

Result<EncodedImage, ImageProcessingError>
encode_avif(const std::vector<std::byte> &source, ImageSizePreset preset,
            const ImageProcessingConfig &config) {
  if (preset == ImageSizePreset::Original) {
    return ImageProcessingError::InvalidPreset;
  }
  if (source.empty()) {
    return ImageProcessingError::DecodeFailed;
  }
  if (!initialize_image_processing()) {
    return ImageProcessingError::InitializationFailed;
  }

  const int dimension = preset_dimension(preset);
  if (dimension <= 0) {
    return ImageProcessingError::InvalidPreset;
  }

  // vips_thumbnail_buffer 只在需要时解码，并把缩放推到解码阶段；
  // height 与 width 同时给出时按框内等比缩放，VIPS_SIZE_DOWN 保证不放大。
  VipsImage *thumbnail = nullptr;
  if (vips_thumbnail_buffer(
          const_cast<std::byte *>(source.data()), source.size(), &thumbnail,
          dimension, "height", dimension, "size", VIPS_SIZE_DOWN,
          "auto_rotate", TRUE, NULL) != 0 ||
      thumbnail == nullptr) {
    log_vips_error("thumbnail failed");
    return ImageProcessingError::DecodeFailed;
  }

  const std::string options = avif_save_options(config);
  void *output = nullptr;
  std::size_t output_size = 0;
  const int result = vips_image_write_to_buffer(
      thumbnail, options.c_str(), &output, &output_size, NULL);
  g_object_unref(thumbnail);

  if (result != 0) {
    log_vips_error("avif encode failed");
    return ImageProcessingError::EncodeFailed;
  }
  if (output == nullptr || output_size == 0) {
    g_free(output);
    return ImageProcessingError::EncodeFailed;
  }

  EncodedImage encoded;
  encoded.bytes.resize(output_size);
  std::memcpy(encoded.bytes.data(), output, output_size);
  g_free(output);
  return encoded;
}

} // namespace tagimg
