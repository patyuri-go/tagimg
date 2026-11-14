#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "job/job.hpp"

namespace tagimg {

// 扫描结果的置信度阈值：general / character 达到该分数才写入。
inline constexpr float kScanTagThreshold = 0.85F;

// 批量新增作品的任务（addArtwork 语义）。
//
// 依次提交 ScanJob 到扫描 worker；每提交一个就返回 Pending 保存现场，
// 依赖完成后由扫描 worker 的回调唤醒。全部实体扫描成功后才在同一个
// 事务里写 artworks / images / artwork_images / artwork_tags /
// artwork_artists。任一实体扫描失败则不写库。
class AddJob final : public Job {
public:
  explicit AddJob(AddRequest request);

  [[nodiscard]] StepResult step(JobContext &context,
                                const JobRun &run) override;

  [[nodiscard]] Result<JobOutcome, JobError> take_outcome() override;

private:
  // 处理已完成的 pending_scan_，把结果并入 scans_ / failures_。
  void collect_scan(JobContext &context);

  // 全部扫描结束后的收尾：合并标签、写库、生成结果。
  void finish(JobContext &context);

  AddRequest request_;
  std::size_t next_index_ = 0;
  std::string pending_entity_id_;
  std::optional<JobId> pending_scan_;
  std::vector<ScanResult> scans_;
  std::vector<ScanFailure> failures_;
  std::optional<Result<JobOutcome, JobError>> outcome_;
};

} // namespace tagimg
