#pragma once

#include <optional>
#include <string>

#include "job/job.hpp"

namespace tagimg {

// 重新扫描一个作品内的全部图片（rescanArtwork 语义）。
//
// 逐张重新下载、打标并生成向量；任意一张失败都不写库，作品的旧向量与
// 旧标签保持原样。全部成功后先按 image_id 替换向量，再把本次扫描出的
// 标签增量加入作品（只增不删）。
class RescanArtworkJob final : public Job {
public:
  explicit RescanArtworkJob(RescanArtworkRequest request);

  [[nodiscard]] StepResult step(JobContext &context,
                                const JobRun &run) override;

  [[nodiscard]] Result<JobOutcome, JobError> take_outcome() override;

private:
  RescanArtworkRequest request_;
  std::optional<Result<JobOutcome, JobError>> outcome_;
};

} // namespace tagimg
