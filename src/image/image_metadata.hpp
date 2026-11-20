#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

#include "common/result.hpp"

namespace tagimg {

// 扫描阶段从图片本身读出的元数据。
//
// 字段都允许为空：图片能打开但缺少对应信息时写 NULL。
struct ImageMetadata {
  std::optional<int> width;
  std::optional<int> height;
  std::optional<int> frame_count;
  std::optional<std::string> mime_type;
  // 相机记录的本地墙上时间，格式 YYYY-MM-DD HH:MM:SS。
  std::optional<std::string> taken_at;
};

enum class ImageMetadataError {
  InitializationFailed,
  OpenFailed,
};

// 根据文件前缀判断 libvips 是否能按图片/动图打开。
//
// 只放行扫描流程支持的格式：JPEG、PNG、GIF、WebP、TIFF、HEIF/AVIF。
// 返回 false 表示不是受支持的图片，调用方应按 NotImage 拒绝。
[[nodiscard]] Result<bool, ImageMetadataError>
probe_supported_image(std::span<const std::byte> prefix);

// 打开文件并读取尺寸、帧数、真实格式与拍摄时间。
//
// libvips 无法打开文件时返回 OpenFailed；能打开但缺某个字段时该字段为空。
[[nodiscard]] Result<ImageMetadata, ImageMetadataError>
read_image_metadata(const std::filesystem::path &path);

} // namespace tagimg
