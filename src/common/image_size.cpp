#include "common/image_size.hpp"

namespace tagimg {

std::optional<ImageSizePreset>
parse_image_preset(std::string_view value) noexcept {
  if (value == "mini") {
    return ImageSizePreset::Mini;
  }
  if (value == "thumb") {
    return ImageSizePreset::Thumb;
  }
  if (value == "small") {
    return ImageSizePreset::Small;
  }
  if (value == "regular") {
    return ImageSizePreset::Regular;
  }
  if (value == "original") {
    return ImageSizePreset::Original;
  }
  return std::nullopt;
}

std::string_view to_string(ImageSizePreset preset) noexcept {
  switch (preset) {
  case ImageSizePreset::Mini:
    return "mini";
  case ImageSizePreset::Thumb:
    return "thumb";
  case ImageSizePreset::Small:
    return "small";
  case ImageSizePreset::Regular:
    return "regular";
  case ImageSizePreset::Original:
    return "original";
  }
  return "unknown";
}

} // namespace tagimg
