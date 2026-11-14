#pragma once

#include <filesystem>
#include <functional>
#include <memory>

#include "job/job.hpp"

namespace tagimg {

// 扫描任务服务：持有扫描 worker 与写库 worker。
//
// REST 与 GraphQL 两个入口共用同一个实例，避免重复建线程、重复占用
// 标签模型。addArtwork 只提交任务并立即返回；任务结束后的结果通过
// on_done 回调交给调用方。任务终态不保存，也不提供任何查询接口。
class ScanService {
public:
  ScanService(std::shared_ptr<const EntityRepository> entity_repository,
              std::shared_ptr<const TagimgRepository> tagimg_repository,
              std::shared_ptr<WdTagger> tagger, std::filesystem::path work_dir);

  ~ScanService();

  ScanService(const ScanService &) = delete;
  ScanService &operator=(const ScanService &) = delete;
  ScanService(ScanService &&) = delete;
  ScanService &operator=(ScanService &&) = delete;

  // 提交一次 add 任务。返回 false 表示 worker 已停止，任务没有入队。
  // on_done 在 worker 线程里执行，可以是空函数。
  bool
  submit(AddRequest request,
         std::function<void(const Result<JobOutcome, JobError> &)> on_done);

  // 停止接收新任务，等待正在执行的任务结束；未完成的直接丢弃。
  void shutdown();

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace tagimg
