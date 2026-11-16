#include "job/rescan_artwork_job.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "common/uuid.hpp"
#include "job/scan_job.hpp"
#include "job/scan_tags.hpp"

namespace tagimg {
namespace {

JobError map_write_error(TagimgRepositoryError error) {
  switch (error) {
  case TagimgRepositoryError::InvalidArgument:
    return JobError::InvalidArgument;
  case TagimgRepositoryError::ConnectionFailed:
  case TagimgRepositoryError::QueryFailed:
    return JobError::WriteFailed;
  case TagimgRepositoryError::DuplicateImage:
    return JobError::DuplicateImage;
  case TagimgRepositoryError::UnknownTag:
    return JobError::UnknownTag;
  case TagimgRepositoryError::UnknownArtist:
    return JobError::UnknownArtist;
  case TagimgRepositoryError::TagHierarchyMissing:
    return JobError::TagHierarchyMissing;
  }
  return JobError::WriteFailed;
}

// 按 ID 查实体时的错误映射：读库失败用 RepositoryFailed，与写库失败区分开。
JobError map_lookup_error(TagimgRepositoryError error) {
  switch (error) {
  case TagimgRepositoryError::InvalidArgument:
    return JobError::InvalidArgument;
  default:
    return JobError::RepositoryFailed;
  }
}

} // namespace

RescanArtworkJob::RescanArtworkJob(RescanArtworkRequest request)
    : request_(std::move(request)) {
  if (!is_uuid(request_.artwork_id)) {
    outcome_ = Result<JobOutcome, JobError>::err(JobError::InvalidArgument);
  }
}

StepResult RescanArtworkJob::step(JobContext &context, const JobRun &run) {
  (void)run;
  if (outcome_) {
    return StepResult::Done;
  }

  const TagimgRepository &repository = context.tagimg_repository.get();

  auto artwork = repository.find_artwork(request_.artwork_id);
  if (!artwork) {
    outcome_ =
        Result<JobOutcome, JobError>::err(map_lookup_error(artwork.error()));
    return StepResult::Done;
  }
  if (!artwork->has_value()) {
    outcome_ = Result<JobOutcome, JobError>::err(JobError::EntityNotFound);
    return StepResult::Done;
  }

  const std::vector<std::string> image_ids = (*artwork)->image_ids;
  if (image_ids.empty()) {
    outcome_ = Result<JobOutcome, JobError>::err(JobError::InvalidArgument);
    return StepResult::Done;
  }

  AddResult result;
  std::vector<ScanResult> scans;
  scans.reserve(image_ids.size());

  // 任何一张失败都没有写库的必要：旧向量与旧标签保持原样。
  for (const std::string &image_id : image_ids) {
    auto entity_id = repository.find_image_entity_id(image_id);
    if (!entity_id) {
      outcome_ =
          Result<JobOutcome, JobError>::err(map_lookup_error(entity_id.error()));
      return StepResult::Done;
    }
    if (!*entity_id) {
      result.failures.push_back(
          ScanFailure{"", JobError::EntityNotFound, image_id});
      break;
    }

    auto scan = ScanJob::rescan_entity(context, **entity_id);
    if (!scan) {
      result.failures.push_back(
          ScanFailure{**entity_id, scan.error(), image_id});
      break;
    }
    scans.push_back(std::move(*scan));
  }

  if (!result.failures.empty()) {
    outcome_ = JobOutcome(std::move(result));
    return StepResult::Done;
  }

  // 扫描全部成功，接下来才动数据库。三件事放在同一个事务里：
  // 替换向量、把本次扫描出的标签增量挂上去、刷新图片扫描时间。
  // 任何一步失败都整体回滚，作品的旧向量与旧标签保持原样。
  std::vector<ImageEmbeddingInput> embeddings;
  embeddings.reserve(scans.size());
  for (std::size_t index = 0; index < scans.size(); ++index) {
    embeddings.push_back(
        ImageEmbeddingInput{image_ids[index], scans[index].embedding});
  }

  std::map<TagKey, float> best_tags;
  for (const ScanResult &scan : scans) {
    merge_tags(best_tags, scan.tags.general, "general", kScanTagThreshold);
    merge_tags(best_tags, scan.tags.character, "character", kScanTagThreshold);
    merge_best_rating(best_tags, scan.tags.rating);
  }
  const std::vector<ArtworkTag> scanned_tags = build_scanned_tags(best_tags);

  std::size_t added_tag_count = 0;
  {
    auto opened = repository.begin_artwork_update(request_.artwork_id);
    if (!opened) {
      outcome_ =
          Result<JobOutcome, JobError>::err(map_write_error(opened.error()));
      return StepResult::Done;
    }
    if (!opened->has_value()) {
      outcome_ = Result<JobOutcome, JobError>::err(JobError::EntityNotFound);
      return StepResult::Done;
    }
    ArtworkUpdateTransaction &txn = **opened;

    // upsert 语义就是替换，不需要先删除旧向量。
    const auto replaced = txn.replace_embeddings(embeddings);
    if (!replaced) {
      outcome_ = Result<JobOutcome, JobError>::err(
          map_write_error(replaced.error()));
      return StepResult::Done;
    }

    // 标签只增不删：本次没扫出来的旧标签继续留在作品上。
    const auto tagged = txn.add_scanned_tags(scanned_tags);
    if (!tagged) {
      outcome_ =
          Result<JobOutcome, JobError>::err(map_write_error(tagged.error()));
      return StepResult::Done;
    }
    added_tag_count = *tagged;

    const auto touched = txn.touch_image_scans(image_ids);
    if (!touched) {
      outcome_ =
          Result<JobOutcome, JobError>::err(map_write_error(touched.error()));
      return StepResult::Done;
    }

    const auto committed = txn.commit();
    if (!committed) {
      outcome_ = Result<JobOutcome, JobError>::err(
          map_write_error(committed.error()));
      return StepResult::Done;
    }
  }

  result.scans = std::move(scans);
  result.artwork_id = request_.artwork_id;
  result.image_ids = image_ids;
  result.tag_count = added_tag_count;
  outcome_ = JobOutcome(std::move(result));
  return StepResult::Done;
}

Result<JobOutcome, JobError> RescanArtworkJob::take_outcome() {
  if (!outcome_) {
    return JobError::InternalError;
  }
  return std::move(*outcome_);
}

} // namespace tagimg
