#pragma once

#include <optional>
#include <string>

#include "job/job.hpp"

namespace tagimg {

// 扫描单个实体：确认是图片、从 WebDAV 下载到临时文件、调用 tagger。
//
// 下载成功后临时文件在扫描结束（对象析构）时自动删除。step() 本身会
// 同步完成下载和推理，因此会占用 worker 的一个线程。
class ScanJob final : public Job {
public:
  explicit ScanJob(std::string entity_id);

  // 扫描实体，并固定检查实体是否已经入库。
  [[nodiscard]] static Result<ScanResult, JobError>
  scan_entity(JobContext &context, const std::string &entity_id);

  // 重新扫描已有图片对应的实体，不检查实体是否已经入库。
  [[nodiscard]] static Result<ScanResult, JobError>
  rescan_entity(JobContext &context, const std::string &entity_id);

  [[nodiscard]] StepResult step(JobContext &context,
                                const JobRun &run) override;

  [[nodiscard]] Result<JobOutcome, JobError> take_outcome() override;

private:
  [[nodiscard]] Result<ScanResult, JobError> scan(JobContext &context,
                                                  bool check_duplicate);

  [[nodiscard]] Result<ScanResult, JobError>
  scan_location(JobContext &context, const WebdavLocation &location);

  std::string entity_id_;
  std::optional<Result<ScanResult, JobError>> result_;
};

} // namespace tagimg
