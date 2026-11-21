#include "job/cache_maintenance.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>

#include "cache/image_cache_keys.hpp"
#include "cache/image_cache_object.hpp"
#include "cache/image_cache_store.hpp"

namespace tagimg {
namespace {

void log_cache_error(std::string_view action, ImageCacheError error) {
  std::cerr << "rescan cache: " << action << ": " << to_string(error) << '\n';
}

void invalidate_variants(JobContext &context, const std::string &image_id) {
  constexpr std::array<ImageSizePreset, 4> kPresets = {
      ImageSizePreset::Mini, ImageSizePreset::Thumb, ImageSizePreset::Small,
      ImageSizePreset::Regular};
  for (const ImageSizePreset preset : kPresets) {
    auto found = context.image_cache->find(
        make_variant_cache_key(image_id, preset, context.image_processing));
    if (!found) {
      log_cache_error("variant find failed", found.error());
      continue;
    }
    if (!*found) {
      continue;
    }
    auto object = std::make_shared<StoredImageCacheObject>(std::move(**found));
    if (!object->remove()) {
      std::cerr << "rescan cache: variant is in use, skipped: " << image_id
                << '\n';
    }
  }
}

} // namespace

void finalize_rescan_cache(JobContext &context,
                           const std::vector<std::string> &image_ids,
                           std::vector<ScanResult> &scans) {
  if (!context.image_cache || !context.image_cache->enabled()) {
    return;
  }
  const std::size_t count = std::min(image_ids.size(), scans.size());
  for (std::size_t index = 0; index < count; ++index) {
    auto &scan = scans[index];
    if (scan.source_cache) {
      const auto committed = scan.source_cache->commit();
      if (!committed) {
        log_cache_error("source commit failed", committed.error());
      } else if (*committed != ImageCacheCommit::Committed) {
        std::cerr << "rescan cache: source skipped because it exceeds the "
                     "limit: "
                  << image_ids[index] << '\n';
      }
      scan.source_cache.reset();
    }
    invalidate_variants(context, image_ids[index]);
  }
}

} // namespace tagimg
