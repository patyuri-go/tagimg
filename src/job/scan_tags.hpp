#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "job/job.hpp"

namespace tagimg {

// wd14 标签的置信度过滤已下沉到 Python 侧：C++ 只负责合并与去重，
// 收到的 general / character 标签都是已经按配置阈值过滤过的。
//
// 同一图片里同名标签只保留最高分；跨图片再按 (分类, 名称) 合并。
using TagKey = std::pair<std::string, std::string>;

inline void merge_tags(std::map<TagKey, float> &best,
                       const std::vector<TagScore> &tags,
                       const std::string &category) {
  for (const TagScore &tag : tags) {
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

// 单张图片的扫描标签：先在本图内合并，再转成仓储层需要的顺序。
inline std::vector<ArtworkTag>
build_image_scanned_tags(const TaggingResult &tags) {
  std::map<TagKey, float> best;
  merge_tags(best, tags.general, "general");
  merge_tags(best, tags.character, "character");
  merge_best_rating(best, tags.rating);
  return build_scanned_tags(best);
}

} // namespace tagimg
