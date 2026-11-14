#include "job/scan_service.hpp"

#include <mutex>
#include <stdexcept>
#include <utility>

#include "job/add_job.hpp"

namespace tagimg {

struct ScanService::Impl {
  Impl(std::shared_ptr<const EntityRepository> entity_repository_ref,
       std::shared_ptr<const TagimgRepository> tagimg_repository_ref,
       std::shared_ptr<WdTagger> tagger_ref, std::filesystem::path work_dir_ref)
      : entity_repository(std::move(entity_repository_ref)),
        tagimg_repository(std::move(tagimg_repository_ref)),
        tagger(std::move(tagger_ref)), work_dir(std::move(work_dir_ref)) {
    JobContext context{std::cref(*entity_repository),
                       std::cref(*tagimg_repository), std::ref(*tagger),
                       work_dir, std::nullopt};
    scan_worker = std::make_unique<JobWorker>(context);
    add_worker = std::make_unique<JobWorker>(context);
    add_worker->set_scan_worker(*scan_worker);
  }

  // 依赖必须比 worker 活得久，声明顺序已保证析构时先停 worker。
  std::shared_ptr<const EntityRepository> entity_repository;
  std::shared_ptr<const TagimgRepository> tagimg_repository;
  std::shared_ptr<WdTagger> tagger;
  std::filesystem::path work_dir;
  std::unique_ptr<JobWorker> scan_worker;
  std::unique_ptr<JobWorker> add_worker;

  mutable std::mutex mutex;
  bool stopping = false;
};

ScanService::ScanService(
    std::shared_ptr<const EntityRepository> entity_repository,
    std::shared_ptr<const TagimgRepository> tagimg_repository,
    std::shared_ptr<WdTagger> tagger, std::filesystem::path work_dir)
    : impl_(std::make_shared<Impl>(std::move(entity_repository),
                                   std::move(tagimg_repository),
                                   std::move(tagger), std::move(work_dir))) {
  if (!impl_->entity_repository || !impl_->tagimg_repository ||
      !impl_->tagger) {
    throw std::invalid_argument("ScanService dependencies must not be null");
  }
}

ScanService::~ScanService() {
  shutdown();
}

bool ScanService::submit(
    AddRequest request,
    std::function<void(const Result<JobOutcome, JobError> &)> on_done) {
  auto impl = impl_;
  {
    std::lock_guard lock(impl->mutex);
    if (impl->stopping) {
      return false;
    }
  }

  const JobId id = impl->add_worker->submit(
      std::make_unique<AddJob>(std::move(request)), false,
      [weak = std::weak_ptr<Impl>(impl),
       on_done = std::move(on_done)](JobId job_id) {
        auto locked = weak.lock();
        if (!locked) {
          return;
        }
        // 结果取走的同时删除记录；终态不会留在 worker 里。
        const auto result = locked->add_worker->take_result(job_id);
        if (on_done) {
          on_done(result);
        }
      });
  return id != 0;
}

void ScanService::shutdown() {
  if (!impl_) {
    return;
  }
  auto impl = impl_;
  {
    std::lock_guard lock(impl->mutex);
    if (impl->stopping) {
      return;
    }
    impl->stopping = true;
  }
  if (impl->add_worker) {
    impl->add_worker->shutdown();
  }
  if (impl->scan_worker) {
    impl->scan_worker->shutdown();
  }
}

} // namespace tagimg
