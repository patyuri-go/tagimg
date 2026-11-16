#include "job/add_job.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

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

// 图片模式下按 ID 查实体时的错误映射：读库失败用 RepositoryFailed，
// 与写库失败区分开。
JobError map_lookup_error(TagimgRepositoryError error) {
  switch (error) {
  case TagimgRepositoryError::InvalidArgument:
    return JobError::InvalidArgument;
  case TagimgRepositoryError::ConnectionFailed:
  case TagimgRepositoryError::QueryFailed:
    return JobError::RepositoryFailed;
  default:
    return JobError::RepositoryFailed;
  }
}

} // namespace

AddJob::AddJob(AddRequest request) : request_(std::move(request)) {
  if (request_.entity_ids.empty() || request_.title.empty()) {
    outcome_ = Result<JobOutcome, JobError>::err(JobError::InvalidArgument);
  }
}

AddJob::AddJob(ImageAddRequest request) : image_request_(std::move(request)) {
  if (image_request_->image_ids.empty() || image_request_->title.empty()) {
    outcome_ = Result<JobOutcome, JobError>::err(JobError::InvalidArgument);
  }
}

StepResult AddJob::step(JobContext &context, const JobRun &run) {
  (void)run;
  if (outcome_) {
    return StepResult::Done;
  }

  // 图片模式不重新扫描时不需要访问存储，直接写库。
  if (image_request_ && !image_request_->rescan) {
    finish(context);
    return StepResult::Done;
  }

  if (image_request_) {
    const TagimgRepository &repository = context.tagimg_repository.get();
    for (const std::string &image_id : image_request_->image_ids) {
      auto entity_id = repository.find_image_entity_id(image_id);
      if (!entity_id) {
        outcome_ = Result<JobOutcome, JobError>::err(
            map_lookup_error(entity_id.error()));
        return StepResult::Done;
      }
      if (!*entity_id) {
        // 图片不存在时记录失败，继续检查后面的图片；最终整单不写库。
        failures_.push_back(
            ScanFailure{"", JobError::EntityNotFound, image_id});
        continue;
      }

      // 已有图片本身就在 images 表中，rescan 时不能按实体查重。
      auto scan = ScanJob::rescan_entity(context, **entity_id);
      if (!scan) {
        failures_.push_back(ScanFailure{**entity_id, scan.error(), image_id});
        continue;
      }
      scans_.push_back(std::move(*scan));
    }

    finish(context);
    return StepResult::Done;
  }

  for (const std::string &entity_id : request_.entity_ids) {
    // 实体模式下在下载前检查是否已入库。
    auto scan = ScanJob::scan_entity(context, entity_id);
    if (!scan) {
      if (scan.error() == JobError::DuplicateImage) {
        outcome_ = Result<JobOutcome, JobError>::err(scan.error());
        return StepResult::Done;
      }
      failures_.push_back(ScanFailure{entity_id, scan.error(), ""});
      continue;
    }
    scans_.push_back(std::move(*scan));
  }

  finish(context);
  return StepResult::Done;
}

void AddJob::finish(JobContext &context) {
  AddResult result;
  if (image_request_) {
    result.image_request = *image_request_;
  } else {
    result.request = request_;
  }
  result.scans = std::move(scans_);
  result.failures = std::move(failures_);

  // 有扫描失败时不写库，避免出现缺图的半个作品。
  if (!result.failures.empty()) {
    outcome_ = JobOutcome(std::move(result));
    return;
  }

  std::map<TagKey, float> best_tags;
  for (const ScanResult &scan : result.scans) {
    merge_tags(best_tags, scan.tags.general, "general", kScanTagThreshold);
    merge_tags(best_tags, scan.tags.character, "character", kScanTagThreshold);
    merge_best_rating(best_tags, scan.tags.rating);
  }

  std::vector<ArtworkTag> scanned_tags = build_scanned_tags(best_tags);

  // scans 与 entity_ids/image_ids 顺序一致，向量按同一顺序交给仓储层。
  std::vector<std::vector<float>> embeddings;
  embeddings.reserve(result.scans.size());
  for (const ScanResult &scan : result.scans) {
    embeddings.push_back(scan.embedding);
  }

  Result<SavedArtwork, TagimgRepositoryError> saved =
      TagimgRepositoryError::InvalidArgument;
  if (image_request_) {
    ImageArtworkInput input;
    input.title = image_request_->title;
    input.image_ids = image_request_->image_ids;
    input.description = image_request_->description;
    input.published_at = image_request_->published_at;
    input.tag_ids = image_request_->tag_ids;
    input.artist_ids = image_request_->artist_ids;
    input.scanned_tags = std::move(scanned_tags);
    input.embeddings = std::move(embeddings);
    input.rescan = image_request_->rescan;
    saved = context.tagimg_repository.get().save_artwork_from_images(input);
  } else {
    ArtworkInput input;
    input.title = request_.title;
    input.description = request_.description;
    input.published_at = request_.published_at;
    input.tag_ids = request_.tag_ids;
    input.artist_ids = request_.artist_ids;
    input.entity_ids = result.request.entity_ids;
    input.scanned_tags = std::move(scanned_tags);
    input.embeddings = std::move(embeddings);
    saved = context.tagimg_repository.get().save_artwork(input);
  }

  if (!saved) {
    outcome_ =
        Result<JobOutcome, JobError>::err(map_write_error(saved.error()));
    return;
  }

  result.artwork_id = std::move(saved->artwork_id);
  result.image_ids = std::move(saved->image_ids);
  result.tag_count = best_tags.size();
  outcome_ = JobOutcome(std::move(result));
}

Result<JobOutcome, JobError> AddJob::take_outcome() {
  if (!outcome_) {
    return JobError::InternalError;
  }
  return std::move(*outcome_);
}

} // namespace tagimg
