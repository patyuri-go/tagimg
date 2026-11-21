#pragma once

#include <filesystem>
#include <functional>
#include <memory>

#include "common/storage.hpp"
#include "job/job.hpp"

namespace tagimg {

class ImageCacheStore;

// 服务运行期持有任务 worker。
//
// REST 与 GraphQL 两个入口共用同一个实例，避免重复建线程、重复占用
// 标签模型。addArtwork 只提交任务并立即返回；任务结束后的结果通过
// on_done 回调交给调用方。任务终态不保存，也不提供任何查询接口。
class ServiceRuntime {
public:
  ServiceRuntime(StorageConfig storage,
                 std::shared_ptr<const TagimgRepository> tagimg_repository,
                 std::shared_ptr<WdTagger> tagger,
                 std::shared_ptr<ClipEmbedder> embedder,
                 std::filesystem::path work_dir,
                 const AiHealthClient &ai_health,
                 std::shared_ptr<ImageCacheStore> image_cache = {},
                 ImageProcessingConfig image_processing = {});

  ~ServiceRuntime();

  ServiceRuntime(const ServiceRuntime &) = delete;
  ServiceRuntime &operator=(const ServiceRuntime &) = delete;
  ServiceRuntime(ServiceRuntime &&) = delete;
  ServiceRuntime &operator=(ServiceRuntime &&) = delete;

  // 提交一次 add 任务。返回 false 表示 worker 已停止，任务没有入队。
  // on_done 在 worker 线程里执行，可以是空函数。
  bool
  submit(AddRequest request,
         std::function<void(const Result<JobOutcome, JobError> &)> on_done);

  // 用已有图片提交一次 add 任务，语义同 AddJob(ImageAddRequest)。
  bool
  submit(ImageAddRequest request,
         std::function<void(const Result<JobOutcome, JobError> &)> on_done);

  // 重新扫描一个作品内的全部图片，语义同 RescanArtworkJob。
  bool
  submit(RescanArtworkRequest request,
         std::function<void(const Result<JobOutcome, JobError> &)> on_done);

  // 停止接收新任务，等待正在执行的任务结束；未完成的直接丢弃。
  void shutdown();

private:
  // 把构造好的 job 交给 worker，并挂上结果回调。
  bool
  submit_job(std::unique_ptr<Job> job,
             std::function<void(const Result<JobOutcome, JobError> &)> on_done);

  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace tagimg
