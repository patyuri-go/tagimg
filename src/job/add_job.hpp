#pragma once

#include <optional>
#include <vector>

#include "job/job.hpp"

namespace tagimg {

// 批量新增作品的任务（addArtwork 语义）。
//
// 在单个 worker 内同步逐张扫描；全部实体扫描成功后，在同一个事务里写
// artworks / images / artwork_images / artwork_tags / artwork_artists。
// 任一实体扫描失败则不写库。
class AddJob final : public Job {
public:
  explicit AddJob(AddRequest request);
  // 用已有图片新建作品；rescan 为 true 时逐张重新扫描。
  explicit AddJob(ImageAddRequest request);

  [[nodiscard]] StepResult step(JobContext &context,
                                const JobRun &run) override;

  [[nodiscard]] Result<JobOutcome, JobError> take_outcome() override;

private:
  // 全部扫描结束后的收尾：合并标签、写库、生成结果。
  void finish(JobContext &context);

  AddRequest request_;
  // 图片模式下有值；此时 request_ 不使用。
  std::optional<ImageAddRequest> image_request_;
  std::vector<ScanResult> scans_;
  std::vector<ScanFailure> failures_;
  std::optional<Result<JobOutcome, JobError>> outcome_;
};

} // namespace tagimg
