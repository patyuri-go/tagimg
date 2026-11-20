#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

#include <vips/vips.h>

namespace tagimg {

// 动图 AVIF 序列编码参数。
struct AnimatedAvifConfig {
  std::uint32_t quality = 65;
  // 编码速度：0 最慢、9 最快，与 imageSpeed 语义一致。
  std::uint32_t speed = 6;
  // 目标外接框边长；0 表示不缩放。
  std::uint32_t max_dimension = 0;
  std::uint32_t max_frames = 300;
  std::uint64_t max_total_pixels = 300'000'000;
  std::uint64_t max_output_bytes = 50 * 1024 * 1024;
};

enum class AnimatedAvifError {
  None,
  TooManyFrames,
  TooManyPixels,
  DecodeFailed,
  EncodeFailed,
  Cancelled,
  OutputTooLarge,
  SinkFailed,
};

[[nodiscard]] std::string_view to_string(AnimatedAvifError error) noexcept;

// 把多页 VipsImage（gifload/webpload 带 n=-1 的结果）编码成动图 AVIF。
//
// 逐页 crop -> 缩放 -> RGBA -> libheif 序列编码，输出通过 chunk_sink 逐块
// 交出，不落盘。cancelled() 返回 true 或 chunk_sink 返回 false 时立即中止。
// 函数同步执行，调用方需保证 image 在调用期间有效。
[[nodiscard]] AnimatedAvifError encode_animated_avif(
    VipsImage *image, const AnimatedAvifConfig &config,
    const std::function<bool()> &cancelled,
    const std::function<bool(const void *, std::size_t)> &chunk_sink);

} // namespace tagimg
