#include "job/service_runtime.hpp"

#include <mutex>
#include <stdexcept>
#include <utility>

#include "job/add_job.hpp"
#include "job/rescan_artwork_job.hpp"

namespace tagimg {

struct ServiceRuntime::Impl {
  Impl(std::shared_ptr<const EntityRepository> entity_repository_ref,
       std::shared_ptr<const TagimgRepository> tagimg_repository_ref,
       std::shared_ptr<WdTagger> tagger_ref,
       std::shared_ptr<ClipEmbedder> embedder_ref,
       std::filesystem::path work_dir_ref,
       const AiHealthClient &ai_health_ref)
      : entity_repository(std::move(entity_repository_ref)),
        tagimg_repository(std::move(tagimg_repository_ref)),
        tagger(std::move(tagger_ref)), embedder(std::move(embedder_ref)),
        ai_health(&ai_health_ref), work_dir(std::move(work_dir_ref)) {
    JobContext context{std::cref(*entity_repository),
                       std::cref(*tagimg_repository), std::ref(*tagger),
                       std::ref(*embedder), std::cref(*ai_health), work_dir};
    worker = std::make_unique<JobWorker>(context);
  }

  // 依赖必须比 worker 活得久，声明顺序已保证析构时先停 worker。
  std::shared_ptr<const EntityRepository> entity_repository;
  std::shared_ptr<const TagimgRepository> tagimg_repository;
  std::shared_ptr<WdTagger> tagger;
  std::shared_ptr<ClipEmbedder> embedder;
  const AiHealthClient *ai_health = nullptr;
  std::filesystem::path work_dir;
  std::unique_ptr<JobWorker> worker;

  mutable std::mutex mutex;
  bool stopping = false;
};

ServiceRuntime::ServiceRuntime(
    std::shared_ptr<const EntityRepository> entity_repository,
    std::shared_ptr<const TagimgRepository> tagimg_repository,
    std::shared_ptr<WdTagger> tagger,
    std::shared_ptr<ClipEmbedder> embedder,
    std::filesystem::path work_dir, const AiHealthClient &ai_health) {
  if (!entity_repository || !tagimg_repository || !tagger || !embedder) {
    throw std::invalid_argument("ServiceRuntime dependencies must not be null");
  }
  impl_ = std::make_shared<Impl>(std::move(entity_repository),
                                 std::move(tagimg_repository),
                                 std::move(tagger), std::move(embedder),
                                 std::move(work_dir), ai_health);
}

ServiceRuntime::~ServiceRuntime() {
  shutdown();
}

bool ServiceRuntime::submit_job(
    std::unique_ptr<Job> job,
    std::function<void(const Result<JobOutcome, JobError> &)> on_done) {
  auto impl = impl_;
  {
    std::lock_guard lock(impl->mutex);
    if (impl->stopping) {
      return false;
    }
  }

  const JobId id =
      impl->worker->submit(std::move(job), false,
                           [weak = std::weak_ptr<Impl>(impl),
                            on_done = std::move(on_done)](JobId job_id) {
                             auto locked = weak.lock();
                             if (!locked) {
                               return;
                             }
                             // 结果取走的同时删除记录；终态不会留在 worker
                             // 里。
                             const auto result =
                                 locked->worker->take_result(job_id);
                             if (on_done) {
                               on_done(result);
                             }
                           });
  return id != 0;
}

bool ServiceRuntime::submit(
    AddRequest request,
    std::function<void(const Result<JobOutcome, JobError> &)> on_done) {
  return submit_job(std::make_unique<AddJob>(std::move(request)),
                    std::move(on_done));
}

bool ServiceRuntime::submit(
    ImageAddRequest request,
    std::function<void(const Result<JobOutcome, JobError> &)> on_done) {
  return submit_job(std::make_unique<AddJob>(std::move(request)),
                    std::move(on_done));
}

bool ServiceRuntime::submit(
    RescanArtworkRequest request,
    std::function<void(const Result<JobOutcome, JobError> &)> on_done) {
  return submit_job(std::make_unique<RescanArtworkJob>(std::move(request)),
                    std::move(on_done));
}

void ServiceRuntime::shutdown() {
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
  if (impl->worker) {
    impl->worker->shutdown();
  }
}

} // namespace tagimg
