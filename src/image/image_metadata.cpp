#include "image/image_metadata.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include <vips/vips.h>

#include "image/image_processor.hpp"

namespace tagimg {
namespace {

constexpr std::string_view kDateTimeOriginal = "exif-ifd2-DateTimeOriginal";
constexpr std::string_view kDateTimeDigitized = "exif-ifd2-DateTimeDigitized";
constexpr std::string_view kDateTime = "exif-ifd0-DateTime";

bool is_supported_loader_class(std::string_view loader) {
  return loader == "VipsForeignLoadJpegBuffer" ||
         loader == "VipsForeignLoadPngBuffer" ||
         loader == "VipsForeignLoadNsgifBuffer" ||
         loader == "VipsForeignLoadWebpBuffer" ||
         loader == "VipsForeignLoadTiffBuffer" ||
         loader == "VipsForeignLoadHeifBuffer";
}

std::string lowercase_metadata_value(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  const std::size_t description = result.find_first_of(" (");
  if (description != std::string::npos) {
    result.resize(description);
  }
  return result;
}

// EXIF 的日期值是 "YYYY:MM:DD HH:MM:SS"；libvips 有时会在后面附加
// "(..., ASCII, ...)" 之类的描述，截断后再解析。
bool is_valid_calendar_date(int year, int month, int day) {
  static constexpr int kDaysPerMonth[] = {31, 28, 31, 30, 31, 30,
                                          31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12 || day < 1) {
    return false;
  }
  int max_day = kDaysPerMonth[month - 1];
  const bool leap_year =
      (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (month == 2 && leap_year) {
    max_day = 29;
  }
  return day <= max_day;
}

std::optional<std::string> parse_exif_datetime(std::string_view raw) {
  const std::size_t description = raw.find(" (");
  if (description != std::string_view::npos) {
    raw = raw.substr(0, description);
  }
  if (raw.size() < 19) {
    return std::nullopt;
  }

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  const std::string value(raw.substr(0, 19));
  if (std::sscanf(value.c_str(), "%d:%d:%d %d:%d:%d", &year, &month, &day,
                  &hour, &minute, &second) != 6) {
    return std::nullopt;
  }
  if (year < 1 || year > 9999 || !is_valid_calendar_date(year, month, day) ||
      hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 ||
      second > 59) {
    return std::nullopt;
  }

  char formatted[32] = {};
  std::snprintf(formatted, sizeof(formatted), "%04d-%02d-%02d %02d:%02d:%02d",
                year, month, day, hour, minute, second);
  return std::string(formatted);
}

std::optional<std::string> read_string_metadata(const VipsImage *image,
                                                const char *name) {
  if (vips_image_get_typeof(image, name) == 0) {
    return std::nullopt;
  }
  char *value = nullptr;
  if (vips_image_get_as_string(image, name, &value) != 0 || value == nullptr) {
    return std::nullopt;
  }
  std::string result(value);
  g_free(value);
  return result;
}

std::optional<std::string> read_taken_at(const VipsImage *image) {
  for (const std::string_view key :
       {kDateTimeOriginal, kDateTimeDigitized, kDateTime}) {
    const auto raw = read_string_metadata(image, std::string(key).c_str());
    if (!raw) {
      continue;
    }
    if (auto parsed = parse_exif_datetime(*raw)) {
      return parsed;
    }
  }
  return std::nullopt;
}

std::optional<std::string> read_mime_type(const VipsImage *image) {
  const char *loader = nullptr;
  if (vips_image_get_string(image, VIPS_META_LOADER, &loader) != 0 ||
      loader == nullptr) {
    return std::nullopt;
  }

  const std::string_view name(loader);
  if (name == "jpegload") {
    return std::string("image/jpeg");
  }
  if (name == "pngload") {
    return std::string("image/png");
  }
  if (name == "gifload") {
    return std::string("image/gif");
  }
  if (name == "webpload") {
    return std::string("image/webp");
  }
  if (name == "tiffload") {
    return std::string("image/tiff");
  }
  if (name == "heifload") {
    const auto compression = read_string_metadata(image, "heif-compression");
    if (compression) {
      const std::string codec = lowercase_metadata_value(*compression);
      if (codec == "av1") {
        return std::string("image/avif");
      }
      if (codec == "hevc") {
        return std::string("image/heic");
      }
    }
    return std::string("image/heif");
  }
  return std::nullopt;
}

int read_orientation(const VipsImage *image) {
  if (vips_image_get_typeof(image, "orientation") == 0) {
    return 1;
  }
  const auto value = read_string_metadata(image, "orientation");
  if (!value) {
    return 1;
  }
  return std::atoi(value->c_str());
}

} // namespace

Result<bool, ImageMetadataError>
probe_supported_image(std::span<const std::byte> prefix) {
  if (!initialize_image_processing()) {
    return ImageMetadataError::InitializationFailed;
  }
  if (prefix.empty()) {
    return false;
  }

  const char *loader = vips_foreign_find_load_buffer(prefix.data(), prefix.size());
  if (loader == nullptr) {
    return false;
  }
  return is_supported_loader_class(loader);
}

Result<ImageMetadata, ImageMetadataError>
read_image_metadata(const std::filesystem::path &path) {
  if (!initialize_image_processing()) {
    return ImageMetadataError::InitializationFailed;
  }

  VipsImage *image = vips_image_new_from_file(path.c_str(), nullptr);
  if (image == nullptr) {
    return ImageMetadataError::OpenFailed;
  }

  ImageMetadata metadata;

  const int width = vips_image_get_width(image);
  const int height = vips_image_get_height(image);
  const int pages = vips_image_get_n_pages(image);
  const int page_height = vips_image_get_page_height(image);

  if (width > 0) {
    metadata.width = width;
  }
  if (pages > 1) {
    metadata.frame_count = pages;
    if (page_height > 0) {
      metadata.height = page_height;
    }
  } else {
    metadata.frame_count = 1;
    if (height > 0) {
      metadata.height = height;
    }
  }

  // 浏览器会按 EXIF orientation 旋转显示；数据库里的尺寸应与其一致。
  const int orientation = read_orientation(image);
  if (orientation >= 5 && orientation <= 8 && metadata.width &&
      metadata.height) {
    std::swap(*metadata.width, *metadata.height);
  }

  metadata.mime_type = read_mime_type(image);
  metadata.taken_at = read_taken_at(image);

  g_object_unref(image);
  return metadata;
}

} // namespace tagimg
