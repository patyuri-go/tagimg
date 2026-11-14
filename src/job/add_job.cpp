#include "job/add_job.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "job/scan_job.hpp"

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

// 同一图片里同名标签只保留最高分；跨图片再按 (分类, 名称) 合并。
using TagKey = std::pair<std::string, std::string>;

void merge_tags(std::map<TagKey, float> &best,
                const std::vector<TagScore> &tags, const std::string &category,
                float threshold) {
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
void merge_best_rating(std::map<TagKey, float> &best,
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

} // namespace

AddJob::AddJob(AddRequest request) : request_(std::move(request)) {
  if (request_.entity_ids.empty() || request_.title.empty()) {
    outcome_ = Result<JobOutcome, JobError>::err(JobError::InvalidArgument);
  }
}

StepResult AddJob::step(JobContext &context, const JobRun &run) {
  if (outcome_) {
    return StepResult::Done;
  }

  if (pending_scan_) {
    collect_scan(context);
    if (outcome_) {
      return StepResult::Done;
    }
    if (pending_scan_) {
      // 扫描还没结束，只是被提前唤醒；继续让出线程。
      return StepResult::Pending;
    }
  }

  if (!context.scan_worker) {
    outcome_ = Result<JobOutcome, JobError>::err(JobError::InternalError);
    return StepResult::Done;
  }

  if (next_index_ >= request_.entity_ids.size()) {
    finish(context);
    return StepResult::Done;
  }

  JobWorker &scan_worker = context.scan_worker->get();
  pending_entity_id_ = request_.entity_ids[next_index_];
  ++next_index_;

  // 扫描完成后由回调唤醒本 job，step() 不会阻塞等它。
  JobWorker &self_worker = run.worker;
  const JobId self_id = run.id;
  pending_scan_ = scan_worker.submit(
      std::make_unique<ScanJob>(pending_entity_id_), true,
      [&worker = self_worker, self_id](JobId) { worker.wake(self_id); });

  if (*pending_scan_ == 0) {
    pending_scan_.reset();
    outcome_ = Result<JobOutcome, JobError>::err(JobError::InternalError);
    return StepResult::Done;
  }

  return StepResult::Pending;
}

void AddJob::collect_scan(JobContext &context) {
  JobWorker &scan_worker = context.scan_worker->get();
  const JobSnapshot snapshot = scan_worker.peek(*pending_scan_);
  if (snapshot.state != JobState::Finished) {
    return;
  }

  auto result = scan_worker.take_result(*pending_scan_);
  pending_scan_.reset();

  if (!result) {
    if (result.error() == JobError::JobNotFinished) {
      outcome_ = Result<JobOutcome, JobError>::err(JobError::InternalError);
      return;
    }
    failures_.push_back(ScanFailure{pending_entity_id_, result.error()});
    return;
  }

  auto *scan = std::get_if<ScanResult>(&*result);
  if (scan == nullptr) {
    outcome_ = Result<JobOutcome, JobError>::err(JobError::InternalError);
    return;
  }
  scans_.push_back(std::move(*scan));
}

void AddJob::finish(JobContext &context) {
  AddResult result;
  result.request = request_;
  result.scans = std::move(scans_);
  result.failures = std::move(failures_);

  // 有实体扫描失败时不写库，避免出现缺图的半个作品。
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

  ArtworkInput input;
  input.title = request_.title;
  input.description = request_.description;
  input.published_at = request_.published_at;
  input.tag_ids = request_.tag_ids;
  input.artist_ids = request_.artist_ids;
  input.entity_ids = result.request.entity_ids;
  input.scanned_tags.reserve(best_tags.size());
  for (const auto &entry : best_tags) {
    input.scanned_tags.push_back(
        ArtworkTag{entry.first.second, entry.first.first});
  }

  auto saved = context.tagimg_repository.get().save_artwork(input);
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
