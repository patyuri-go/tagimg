#include "image/animated_avif.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include <libheif/heif.h>
#include <libheif/heif_sequences.h>

namespace tagimg {
namespace {

struct HeifContextDeleter {
  void operator()(heif_context *p) const noexcept { heif_context_free(p); }
};
struct HeifEncoderDeleter {
  void operator()(heif_encoder *p) const noexcept { heif_encoder_release(p); }
};
struct HeifTrackDeleter {
  void operator()(heif_track *p) const noexcept { heif_track_release(p); }
};
struct HeifTrackOptionsDeleter {
  void operator()(heif_track_options *p) const noexcept {
    heif_track_options_release(p);
  }
};
struct HeifSequenceOptionsDeleter {
  void operator()(heif_sequence_encoding_options *p) const noexcept {
    heif_sequence_encoding_options_release(p);
  }
};
struct HeifImageDeleter {
  void operator()(heif_image *p) const noexcept { heif_image_release(p); }
};
struct VipsImageDeleter {
  void operator()(VipsImage *p) const noexcept {
    if (p != nullptr) {
      g_object_unref(p);
    }
  }
};

using HeifContextPtr = std::unique_ptr<heif_context, HeifContextDeleter>;
using HeifEncoderPtr = std::unique_ptr<heif_encoder, HeifEncoderDeleter>;
using HeifTrackPtr = std::unique_ptr<heif_track, HeifTrackDeleter>;
using HeifTrackOptionsPtr =
    std::unique_ptr<heif_track_options, HeifTrackOptionsDeleter>;
using HeifSequenceOptionsPtr =
    std::unique_ptr<heif_sequence_encoding_options, HeifSequenceOptionsDeleter>;
using HeifImagePtr = std::unique_ptr<heif_image, HeifImageDeleter>;
using VipsImagePtr = std::unique_ptr<VipsImage, VipsImageDeleter>;

// 给 libheif 的写出回调：逐块交给调用方，同时检查取消和输出上限。
struct WriterState {
  const std::function<bool(const void *, std::size_t)> *sink = nullptr;
  const std::function<bool()> *cancelled = nullptr;
  std::uint64_t written = 0;
  std::uint64_t limit = 0;

  enum class Failure { None, Cancelled, TooLarge, Sink };
  Failure failure = Failure::None;
};

heif_error write_chunk(heif_context * /*ctx*/, const void *data,
                       std::size_t size, void *userdata) {
  auto &state = *static_cast<WriterState *>(userdata);
  if (state.cancelled != nullptr && (*state.cancelled)()) {
    state.failure = WriterState::Failure::Cancelled;
    return heif_error{heif_error_Encoding_error, heif_suberror_Unspecified,
                      "cancelled"};
  }
  if (state.limit != 0 && state.written + size > state.limit) {
    state.failure = WriterState::Failure::TooLarge;
    return heif_error{heif_error_Encoding_error, heif_suberror_Unspecified,
                      "output too large"};
  }
  if (!(*state.sink)(data, size)) {
    state.failure = WriterState::Failure::Sink;
    return heif_error{heif_error_Encoding_error, heif_suberror_Unspecified,
                      "sink failed"};
  }
  state.written += size;
  return heif_error{heif_error_Ok, heif_suberror_Unspecified, nullptr};
}

std::vector<int> read_delays(VipsImage *image) {
  if (vips_image_get_typeof(image, "delay") == 0) {
    return {};
  }
  // 数组内存属于图像头，不能 g_free；先持有 GValue，拷贝完再 unset。
  GValue value = G_VALUE_INIT;
  if (vips_image_get(image, "delay", &value) != 0) {
    return {};
  }
  std::vector<int> delays;
  if (G_VALUE_TYPE(&value) == VIPS_TYPE_ARRAY_INT) {
    int count = 0;
    const int *values = vips_value_get_array_int(&value, &count);
    if (values != nullptr && count > 0) {
      delays.assign(values, values + count);
    }
  }
  g_value_unset(&value);
  return delays;
}

int read_loop(VipsImage *image) {
  if (vips_image_get_typeof(image, "loop") == 0) {
    return 0;
  }
  int loop = 0;
  if (vips_image_get_int(image, "loop", &loop) != 0) {
    return 0;
  }
  return loop;
}

} // namespace

std::string_view to_string(AnimatedAvifError error) noexcept {
  switch (error) {
  case AnimatedAvifError::None:
    return "ok";
  case AnimatedAvifError::TooManyFrames:
    return "too many frames";
  case AnimatedAvifError::TooManyPixels:
    return "too many pixels";
  case AnimatedAvifError::DecodeFailed:
    return "decode failed";
  case AnimatedAvifError::EncodeFailed:
    return "encode failed";
  case AnimatedAvifError::Cancelled:
    return "cancelled";
  case AnimatedAvifError::OutputTooLarge:
    return "output too large";
  case AnimatedAvifError::SinkFailed:
    return "sink failed";
  }
  return "unknown";
}

AnimatedAvifError encode_animated_avif(
    VipsImage *image, const AnimatedAvifConfig &config,
    const std::function<bool()> &cancelled,
    const std::function<bool(const void *, std::size_t)> &chunk_sink) {
  if (image == nullptr || !cancelled || !chunk_sink) {
    return AnimatedAvifError::DecodeFailed;
  }

  const int width = vips_image_get_width(image);
  const int pages = vips_image_get_n_pages(image);
  const int page_height = vips_image_get_page_height(image);
  if (width <= 0 || pages <= 0 || page_height <= 0) {
    return AnimatedAvifError::DecodeFailed;
  }
  if (static_cast<std::uint64_t>(pages) >
      static_cast<std::uint64_t>(config.max_frames)) {
    return AnimatedAvifError::TooManyFrames;
  }
  const auto total_pixels = static_cast<std::uint64_t>(width) *
                            static_cast<std::uint64_t>(page_height) *
                            static_cast<std::uint64_t>(pages);
  if (total_pixels > config.max_total_pixels) {
    return AnimatedAvifError::TooManyPixels;
  }

  // 缩放到目标外接框，保持宽高比。缩小用 vips_reduce（可流式处理），
  // 避免 vips_resize 内部可能触发的随机访问。
  double hshrink = 1.0;
  double vshrink = 1.0;
  if (config.max_dimension > 0) {
    const int longest = std::max(width, page_height);
    if (longest > static_cast<int>(config.max_dimension)) {
      const double scale = static_cast<double>(config.max_dimension) /
                           static_cast<double>(longest);
      const int target_w =
          std::max(1, static_cast<int>(std::lround(width * scale)));
      const int target_h =
          std::max(1, static_cast<int>(std::lround(page_height * scale)));
      hshrink = static_cast<double>(width) / static_cast<double>(target_w);
      vshrink = static_cast<double>(page_height) / static_cast<double>(target_h);
    }
  }

  const std::vector<int> delays = read_delays(image);
  const int loop = read_loop(image);

  const auto frame_duration = [&delays](int index) -> std::uint32_t {
    int delay = 100;
    if (!delays.empty()) {
      const auto position = std::min<std::size_t>(
          static_cast<std::size_t>(index), delays.size() - 1);
      delay = delays[position];
    }
    // 极短的帧在浏览器里几乎不可见，统一抬到 100ms。
    if (delay <= 10) {
      delay = 100;
    }
    if (delay < 1) {
      delay = 1;
    }
    return static_cast<std::uint32_t>(delay);
  };

  const auto prepare_page = [&](int index) -> VipsImagePtr {
    VipsImage *page_raw = nullptr;
    if (vips_crop(image, &page_raw, 0, index * page_height, width, page_height,
                  nullptr) != 0) {
      return {};
    }
    VipsImagePtr page(page_raw);
    if (hshrink > 1.0 || vshrink > 1.0) {
      VipsImage *reduced_raw = nullptr;
      if (vips_reduce(page.get(), &reduced_raw, hshrink, vshrink, nullptr) !=
          0) {
        return {};
      }
      page.reset(reduced_raw);
    }
    VipsImage *srgb_raw = nullptr;
    if (vips_colourspace(page.get(), &srgb_raw, VIPS_INTERPRETATION_sRGB,
                         nullptr) != 0) {
      return {};
    }
    VipsImagePtr srgb(srgb_raw);
    if (vips_image_get_bands(srgb.get()) >= 4) {
      return srgb;
    }
    VipsImage *rgba_raw = nullptr;
    if (vips_addalpha(srgb.get(), &rgba_raw, nullptr) != 0) {
      return {};
    }
    return VipsImagePtr(rgba_raw);
  };

  VipsImagePtr first = prepare_page(0);
  if (!first) {
    return AnimatedAvifError::DecodeFailed;
  }
  const int out_width = vips_image_get_width(first.get());
  const int out_height = vips_image_get_height(first.get());
  if (out_width <= 0 || out_height <= 0 || out_width > 65535 ||
      out_height > 65535) {
    return AnimatedAvifError::DecodeFailed;
  }

  HeifContextPtr context(heif_context_alloc());
  if (!context) {
    return AnimatedAvifError::EncodeFailed;
  }
  heif_encoder *encoder_raw = nullptr;
  {
    const heif_error error = heif_context_get_encoder_for_format(
        context.get(), heif_compression_AV1, &encoder_raw);
    if (error.code != heif_error_Ok) {
      return AnimatedAvifError::EncodeFailed;
    }
  }
  HeifEncoderPtr encoder(encoder_raw);
  heif_encoder_set_lossy_quality(encoder.get(),
                                 static_cast<int>(config.quality));
  // imageSpeed 与 imageAnimatedSpeed 都是“越大越快”（0 最慢、9 最快），
  // 与 libheif 的 speed 参数方向一致，直接传入即可。
  heif_encoder_set_parameter_integer(
      encoder.get(), "speed",
      static_cast<int>(std::min<std::uint32_t>(config.speed, 9)));

  HeifTrackOptionsPtr track_options(heif_track_options_alloc());
  if (!track_options) {
    return AnimatedAvifError::EncodeFailed;
  }
  heif_track_options_set_timescale(track_options.get(), 1000);

  HeifSequenceOptionsPtr sequence_options(
      heif_sequence_encoding_options_alloc());
  if (!sequence_options) {
    return AnimatedAvifError::EncodeFailed;
  }
  sequence_options->gop_structure = heif_sequence_gop_structure_intra_only;
  sequence_options->save_alpha_channel = 1;
  sequence_options->content_kind =
      heif_sequence_content_kind_image_sequence;

  heif_track *track_raw = nullptr;
  {
    const heif_error error = heif_context_add_visual_sequence_track(
        context.get(), static_cast<std::uint16_t>(out_width),
        static_cast<std::uint16_t>(out_height),
        heif_track_type_image_sequence, track_options.get(),
        sequence_options.get(), &track_raw);
    if (error.code != heif_error_Ok) {
      return AnimatedAvifError::EncodeFailed;
    }
  }
  HeifTrackPtr track(track_raw);

  const auto encode_page = [&](VipsImage *rgba,
                               std::uint32_t duration) -> AnimatedAvifError {
    if (cancelled()) {
      return AnimatedAvifError::Cancelled;
    }
    const int bands = vips_image_get_bands(rgba);
    if (bands != 4 || vips_image_get_width(rgba) != out_width ||
        vips_image_get_height(rgba) != out_height) {
      return AnimatedAvifError::DecodeFailed;
    }
    std::size_t memory_size = 0;
    void *memory = vips_image_write_to_memory(rgba, &memory_size);
    if (memory == nullptr) {
      return AnimatedAvifError::DecodeFailed;
    }
    const auto expected = static_cast<std::size_t>(out_width) *
                          static_cast<std::size_t>(out_height) * 4;
    if (memory_size < expected) {
      g_free(memory);
      return AnimatedAvifError::DecodeFailed;
    }

    heif_image *frame_raw = nullptr;
    {
      const heif_error error =
          heif_image_create(out_width, out_height, heif_colorspace_RGB,
                            heif_chroma_interleaved_RGBA, &frame_raw);
      if (error.code != heif_error_Ok) {
        g_free(memory);
        return AnimatedAvifError::EncodeFailed;
      }
    }
    HeifImagePtr frame(frame_raw);
    {
      const heif_error error = heif_image_add_plane(
          frame.get(), heif_channel_interleaved, out_width, out_height, 8);
      if (error.code != heif_error_Ok) {
        g_free(memory);
        return AnimatedAvifError::EncodeFailed;
      }
    }
    int stride = 0;
    auto *destination = heif_image_get_plane(
        frame.get(), heif_channel_interleaved, &stride);
    if (destination == nullptr || stride <= 0) {
      g_free(memory);
      return AnimatedAvifError::EncodeFailed;
    }
    const auto *source = static_cast<const std::uint8_t *>(memory);
    const auto row_bytes = static_cast<std::size_t>(out_width) * 4;
    for (int y = 0; y < out_height; ++y) {
      std::memcpy(destination + static_cast<std::size_t>(y) * stride,
                  source + static_cast<std::size_t>(y) * row_bytes, row_bytes);
    }
    g_free(memory);

    heif_image_set_duration(frame.get(), duration);
    {
      const heif_error error = heif_track_encode_sequence_image(
          track.get(), frame.get(), encoder.get(), sequence_options.get());
      if (error.code != heif_error_Ok) {
        return AnimatedAvifError::EncodeFailed;
      }
    }
    return AnimatedAvifError::None;
  };

  {
    VipsImagePtr first_page = std::move(first);
    const auto error = encode_page(first_page.get(), frame_duration(0));
    if (error != AnimatedAvifError::None) {
      return error;
    }
  }
  for (int index = 1; index < pages; ++index) {
    if (cancelled()) {
      return AnimatedAvifError::Cancelled;
    }
    VipsImagePtr page = prepare_page(index);
    if (!page) {
      return AnimatedAvifError::DecodeFailed;
    }
    const auto error = encode_page(page.get(), frame_duration(index));
    if (error != AnimatedAvifError::None) {
      return error;
    }
  }

  {
    const heif_error error =
        heif_track_encode_end_of_sequence(track.get(), encoder.get());
    if (error.code != heif_error_Ok) {
      return AnimatedAvifError::EncodeFailed;
    }
  }
  heif_context_set_number_of_sequence_repetitions(
      context.get(), loop > 0 ? static_cast<std::uint32_t>(loop)
                              : heif_sequence_maximum_number_of_repetitions);

  WriterState state;
  state.sink = &chunk_sink;
  state.cancelled = &cancelled;
  state.limit = config.max_output_bytes;
  heif_writer writer{1, &write_chunk};
  const heif_error write_error =
      heif_context_write(context.get(), &writer, &state);
  if (write_error.code != heif_error_Ok) {
    switch (state.failure) {
    case WriterState::Failure::Cancelled:
      return AnimatedAvifError::Cancelled;
    case WriterState::Failure::TooLarge:
      return AnimatedAvifError::OutputTooLarge;
    case WriterState::Failure::Sink:
      return AnimatedAvifError::SinkFailed;
    case WriterState::Failure::None:
      break;
    }
    return AnimatedAvifError::EncodeFailed;
  }
  return AnimatedAvifError::None;
}

} // namespace tagimg
