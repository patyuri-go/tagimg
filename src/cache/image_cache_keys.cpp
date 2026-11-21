#include "cache/image_cache_keys.hpp"

#include <string>

namespace tagimg {

ImageCacheKey make_source_cache_key(std::string_view image_id,
                                    std::string_view content_type) {
  ImageCacheKey key;
  key.image_id = std::string(image_id);
  key.params.preset = ImageSizePreset::Original;
  key.params.extension = "bin";
  key.params.content_type = std::string(content_type);
  return key;
}

ImageCacheKey make_variant_cache_key(std::string_view image_id,
                                     ImageSizePreset preset,
                                     const ImageProcessingConfig &processing) {
  ImageCacheKey key;
  key.image_id = std::string(image_id);
  key.params.preset = preset;
  key.params.quality = processing.quality;
  key.params.speed = processing.speed;
  key.params.animated_quality = processing.animated_quality;
  key.params.animated_speed = processing.animated_speed;
  key.params.animated_max_frames = processing.animated_max_frames;
  key.params.animated_max_total_pixels = processing.animated_max_total_pixels;
  key.params.max_output_bytes = processing.animated_max_output_bytes;
  key.params.extension = "avif";
  key.params.content_type = "image/avif";
  return key;
}

} // namespace tagimg
