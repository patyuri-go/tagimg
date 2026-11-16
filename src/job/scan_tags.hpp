#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "job/job.hpp"

namespace tagimg {

// 扫描结果的置信度阈值：general / character 达到该分数才写入。
inline constexpr float kScanTagThreshold = 0.85F;

// 同一图片里同名标签只保留最高分；跨图片再按 (分类, 名称) 合并。
using TagKey = std::pair<std::string, std::string>;

inline void merge_tags(std::map<TagKey, float> &best,
                       const std::vector<TagScore> &tags,
                       const std::string &category, float threshold) {
  for (const TagScore &tag : tags) {
    if (tag.score < threshold) {
      continue;
    }
    const TagKey key{category, tag.name};
    const auto existing = best.find(key);
    if (existing == best.end() || existing->second < tag.score) {
      best[key] = tag.score;
    }
  }
}

// rating 只有 4 个候选，概率通常达不到 0.85，因此取概率最高的一个。
inline void merge_best_rating(std::map<TagKey, float> &best,
                              const std::vector<TagScore> &ratings) {
  const auto best_rating =
      std::max_element(ratings.begin(), ratings.end(),
                       [](const TagScore &left, const TagScore &right) {
                         return left.score < right.score;
                       });
  if (best_rating == ratings.end() || best_rating->score <= 0.0F) {
    return;
  }
  const TagKey key{"rating", best_rating->name};
  const auto existing = best.find(key);
  if (existing == best.end() || existing->second < best_rating->score) {
    best[key] = best_rating->score;
  }
}

// 把合并结果按 (分类, 名称) 顺序转成仓储层需要的扫描标签。
inline std::vector<ArtworkTag>
build_scanned_tags(const std::map<TagKey, float> &best) {
  std::vector<ArtworkTag> tags;
  tags.reserve(best.size());
  for (const auto &entry : best) {
    tags.push_back(ArtworkTag{entry.first.second, entry.first.first});
  }
  return tags;
}

} // namespace tagimg
