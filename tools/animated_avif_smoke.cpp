#include <cstdio>
#include <cstdlib>

#include <vips/vips.h>

#include "image/animated_avif.hpp"

// 手动验证工具：把 GIF / 动图 WebP 转成动图 AVIF，打印帧数、时长、循环次数。
// 用法：animated_avif_smoke <input> <output.avif> [max_dimension] [quality]
//       [speed]（0 最慢、9 最快）
int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: %s <input.gif|input.webp> <output.avif> "
                 "[max_dimension] [quality] [speed 0-9, larger is faster]\n",
                 argv[0]);
    return 2;
  }
  if (vips_init(argv[0]) != 0) {
    std::fprintf(stderr, "vips_init failed\n");
    return 2;
  }

  VipsImage *image = vips_image_new_from_file(
      argv[1], "n", -1, "access", VIPS_ACCESS_SEQUENTIAL, nullptr);
  if (image == nullptr) {
    std::fprintf(stderr, "load failed: %s\n", vips_error_buffer());
    vips_shutdown();
    return 1;
  }

  const int pages = vips_image_get_n_pages(image);
  const int page_height = vips_image_get_page_height(image);
  std::printf("input: %dx%d, pages=%d, page_height=%d, bands=%d\n",
              vips_image_get_width(image), vips_image_get_height(image), pages,
              page_height, vips_image_get_bands(image));

  int loop = 0;
  if (vips_image_get_typeof(image, "loop") != 0) {
    vips_image_get_int(image, "loop", &loop);
  }
  std::printf("loop=%d\n", loop);
  if (vips_image_get_typeof(image, "delay") != 0) {
    GValue value = G_VALUE_INIT;
    if (vips_image_get(image, "delay", &value) == 0) {
      if (G_VALUE_TYPE(&value) == VIPS_TYPE_ARRAY_INT) {
        int count = 0;
        const int *delays = vips_value_get_array_int(&value, &count);
        std::printf("delays(ms):");
        for (int i = 0; i < count; ++i) {
          std::printf(" %d", delays[i]);
        }
        std::printf("\n");
      }
      g_value_unset(&value);
    }
  }

  FILE *output = std::fopen(argv[2], "wb");
  if (output == nullptr) {
    std::fprintf(stderr, "cannot open output\n");
    g_object_unref(image);
    vips_shutdown();
    return 1;
  }

  tagimg::AnimatedAvifConfig config;
  if (argc > 3) {
    config.max_dimension = static_cast<std::uint32_t>(std::atoi(argv[3]));
  }
  if (argc > 4) {
    config.quality = static_cast<std::uint32_t>(std::atoi(argv[4]));
  }
  if (argc > 5) {
    config.speed = static_cast<std::uint32_t>(std::atoi(argv[5]));
  }

  const auto result = tagimg::encode_animated_avif(
      image, config, [] { return false; },
      [output](const void *data, std::size_t size) {
        return std::fwrite(data, 1, size, output) == size;
      });
  std::fclose(output);
  g_object_unref(image);

  if (result != tagimg::AnimatedAvifError::None) {
    std::fprintf(stderr, "encode failed: %.*s\n",
                 static_cast<int>(tagimg::to_string(result).size()),
                 tagimg::to_string(result).data());
    std::remove(argv[2]);
    vips_shutdown();
    return 1;
  }
  std::printf("ok: %s\n", argv[2]);
  vips_shutdown();
  return 0;
}
