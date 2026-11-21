#include "cache/image_cache_key.hpp"

#include <algorithm>
#include <cctype>
#include <string>

#include <openssl/evp.h>

namespace tagimg {
namespace {

bool is_valid_image_id(const std::string &image_id) noexcept {
  if (image_id.empty() || image_id.size() > 128) {
    return false;
  }
  for (const unsigned char ch : image_id) {
    if (ch == '|' || ch < 0x20 || ch == 0x7f) {
      return false;
    }
  }
  return true;
}

bool is_valid_extension(const std::string &extension) noexcept {
  if (extension.empty() || extension.size() > 8) {
    return false;
  }
  for (const unsigned char ch : extension) {
    if (!std::islower(ch) && !std::isdigit(ch)) {
      return false;
    }
  }
  return true;
}

} // namespace

int preset_priority(ImageSizePreset preset) noexcept {
  switch (preset) {
  case ImageSizePreset::Original:
    return 0;
  case ImageSizePreset::Regular:
    return 10;
  case ImageSizePreset::Small:
    return 20;
  case ImageSizePreset::Thumb:
    return 30;
  case ImageSizePreset::Mini:
    return 40;
  }
  return 40;
}

std::string normalize_extension(std::string_view extension) {
  std::string normalized;
  if (extension.empty()) {
    return "bin";
  }
  normalized.reserve(extension.size());
  for (const unsigned char ch : extension) {
    normalized.push_back(static_cast<char>(std::tolower(ch)));
  }
  return normalized;
}

bool is_valid_cache_key(const ImageCacheKey &key) noexcept {
  if (!is_valid_image_id(key.image_id)) {
    return false;
  }
  return is_valid_extension(normalize_extension(key.params.extension));
}

std::string canonical_cache_key(const ImageCacheKey &key) {
  const std::string extension = normalize_extension(key.params.extension);
  std::string canonical;
  canonical.reserve(160);
  canonical.append(kImageCacheFormatVersion);
  canonical.push_back('|');
  canonical.append(kImageCacheType);
  canonical.push_back('|');
  canonical.append(key.image_id);
  canonical.append("|preset=");
  canonical.append(to_string(key.params.preset));
  canonical.append("|q=");
  canonical.append(std::to_string(key.params.quality));
  canonical.append("|s=");
  canonical.append(std::to_string(key.params.speed));
  canonical.append("|aq=");
  canonical.append(std::to_string(key.params.animated_quality));
  canonical.append("|as=");
  canonical.append(std::to_string(key.params.animated_speed));
  canonical.append("|mf=");
  canonical.append(std::to_string(key.params.animated_max_frames));
  canonical.append("|mtp=");
  canonical.append(std::to_string(key.params.animated_max_total_pixels));
  canonical.append("|mob=");
  canonical.append(std::to_string(key.params.max_output_bytes));
  canonical.append("|ext=");
  canonical.append(extension);
  return canonical;
}

std::string cache_key_hash(std::string_view canonical) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_length = 0;

  EVP_MD_CTX *context = EVP_MD_CTX_new();
  if (context == nullptr) {
    return {};
  }
  const bool ok =
      EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
      EVP_DigestUpdate(context, canonical.data(), canonical.size()) == 1 &&
      EVP_DigestFinal_ex(context, digest, &digest_length) == 1;
  EVP_MD_CTX_free(context);
  if (!ok || digest_length < 16) {
    return {};
  }

  static constexpr char kHex[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(32);
  for (unsigned int i = 0; i < 16; ++i) {
    hex.push_back(kHex[digest[i] >> 4]);
    hex.push_back(kHex[digest[i] & 0x0f]);
  }
  return hex;
}

std::string_view infer_content_type(std::string_view extension) noexcept {
  if (extension == "avif") {
    return "image/avif";
  }
  if (extension == "jpg" || extension == "jpeg") {
    return "image/jpeg";
  }
  if (extension == "png") {
    return "image/png";
  }
  if (extension == "webp") {
    return "image/webp";
  }
  if (extension == "gif") {
    return "image/gif";
  }
  return "application/octet-stream";
}

} // namespace tagimg
