// 验证 libvips 的 AVIF 编解码链路：生成一张 PNG，按各预设缩放并写 AVIF，
// 再把结果读回来检查尺寸。不依赖数据库和 AI sidecar。
#include <cstddef>
#include <cstring>
#include <iostream>
#include <vector>

#include <vips/vips.h>

#include "image/image_processor.hpp"

namespace {

struct PresetCase {
  tagimg::ImageSizePreset preset;
  int max_dimension;
};

bool is_avif(const std::vector<std::byte> &bytes) {
  if (bytes.size() < 12) {
    return false;
  }
  const auto *raw = reinterpret_cast<const char *>(bytes.data());
  if (std::memcmp(raw + 4, "ftyp", 4) != 0) {
    return false;
  }
  return std::memcmp(raw + 8, "avif", 4) == 0 ||
         std::memcmp(raw + 8, "avis", 4) == 0;
}

} // namespace

int main() {
  if (!tagimg::initialize_image_processing()) {
    std::cerr << "vips init failed\n";
    return 1;
  }

  VipsImage *image = nullptr;
  if (vips_black(&image, 640, 480, NULL) != 0) {
    std::cerr << "vips_black failed: " << vips_error_buffer() << '\n';
    return 1;
  }

  void *png = nullptr;
  std::size_t png_size = 0;
  if (vips_image_write_to_buffer(image, ".png", &png, &png_size, NULL) != 0) {
    std::cerr << "png encode failed: " << vips_error_buffer() << '\n';
    g_object_unref(image);
    return 1;
  }
  g_object_unref(image);

  std::vector<std::byte> source(png_size);
  std::memcpy(source.data(), png, png_size);
  g_free(png);

  const tagimg::ImageProcessingConfig config;
  const PresetCase cases[] = {
      {tagimg::ImageSizePreset::Mini, 48},
      {tagimg::ImageSizePreset::Thumb, 250},
      {tagimg::ImageSizePreset::Small, 540},
      {tagimg::ImageSizePreset::Regular, 1200},
  };

  for (const PresetCase &item : cases) {
    const auto encoded =
        tagimg::encode_avif(source, item.preset, config);
    if (!encoded) {
      std::cerr << "encode failed for "
                << tagimg::to_string(item.preset) << ": "
                << tagimg::to_string(encoded.error()) << '\n';
      return 1;
    }
    if (!is_avif(encoded->bytes)) {
      std::cerr << "output is not avif for "
                << tagimg::to_string(item.preset) << '\n';
      return 1;
    }

    VipsImage *decoded = vips_image_new_from_buffer(
        encoded->bytes.data(), encoded->bytes.size(), "", NULL);
    if (decoded == nullptr) {
      std::cerr << "avif decode failed for "
                << tagimg::to_string(item.preset) << ": "
                << vips_error_buffer() << '\n';
      return 1;
    }
    const int width = vips_image_get_width(decoded);
    const int height = vips_image_get_height(decoded);
    g_object_unref(decoded);

    std::cout << tagimg::to_string(item.preset) << ": "
              << encoded->bytes.size() << " bytes, " << width << 'x'
              << height << '\n';

    if (width <= 0 || height <= 0 || width > item.max_dimension ||
        height > item.max_dimension) {
      std::cerr << "unexpected dimensions for "
                << tagimg::to_string(item.preset) << '\n';
      return 1;
    }
    // 源图只有 640x480，regular 不应放大。
    if (item.preset == tagimg::ImageSizePreset::Regular &&
        (width != 640 || height != 480)) {
      std::cerr << "regular upscaled unexpectedly\n";
      return 1;
    }
  }

  std::cout << "vips smoke ok\n";
  return 0;
}
