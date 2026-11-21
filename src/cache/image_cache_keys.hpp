#pragma once

#include <string>
#include <string_view>

#include "cache/image_cache_key.hpp"
#include "common/image_size.hpp"
#include "image/image_processor.hpp"

namespace tagimg {

// 源缓存：preset=Original，扩展名固定 bin，真实 MIME 放在 content_type。
[[nodiscard]] ImageCacheKey
make_source_cache_key(std::string_view image_id,
                      std::string_view content_type = {});

// 变体缓存：preset + 当前配置的全部编码参数，扩展名固定 avif。
[[nodiscard]] ImageCacheKey
make_variant_cache_key(std::string_view image_id, ImageSizePreset preset,
                       const ImageProcessingConfig &processing);

} // namespace tagimg
