#include "job/job.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace tagimg {
namespace {

struct Record {
  std::unique_ptr<Job> job;
  JobState state = JobState::Queued;
  bool want_result = false;
  JobDoneCallback on_done;
  std::optional<Result<JobOutcome, JobError>> outcome;
};

} // namespace

struct JobWorker::Impl {
  Impl(JobWorker &owner_ref, JobContext worker_context)
      : owner(owner_ref), context(std::move(worker_context)) {
  }

  JobWorker &owner;
  JobContext context;

  mutable std::mutex mutex;
  // 有 job 进入 ready 队列时唤醒工作线程。
  std::condition_variable work_cv;
  // 有 job 完成时唤醒 wait() 的调用方。
  std::condition_variable done_cv;

  std::deque<JobId> ready;
  std::unordered_map<JobId, std::shared_ptr<Record>> records;
  std::vector<std::thread> threads;
  JobId next_id = 1;
  bool stopping = false;

  void run();
};

void JobWorker::Impl::run() {
  while (true) {
    std::shared_ptr<Record> record;
    JobId id = 0;

    {
      std::unique_lock lock(mutex);
      work_cv.wait(lock, [this] { return stopping || !ready.empty(); });
      if (stopping && ready.empty()) {
        return;
      }
      if (ready.empty()) {
        continue;
      }

      id = ready.front();
      ready.pop_front();

      const auto found = records.find(id);
      if (found == records.end()) {
        continue;
      }
      record = found->second;
      if (record->state != JobState::Queued) {
        continue;
      }
      // Running 标记保证同一个 job 不会被并发 step。
      record->state = JobState::Running;
    }

    StepResult step = StepResult::Done;
    std::optional<Result<JobOutcome, JobError>> outcome;

    try {
      step = record->job->step(context, JobRun{id, owner});
      if (step == StepResult::Done) {
        outcome = record->job->take_outcome();
      }
    } catch (...) {
      // job 自己抛异常时转成 InternalError，worker 线程不能退出。
      step = StepResult::Done;
      outcome = Result<JobOutcome, JobError>::err(JobError::InternalError);
    }

    JobDoneCallback callback;
    const bool finished = step == StepResult::Done;
    bool keep_record = false;

    {
      std::lock_guard lock(mutex);
      if (!finished) {
        record->state = JobState::Waiting;
      } else {
        record->state = JobState::Finished;
        record->outcome = std::move(outcome);
        callback = std::move(record->on_done);
        record->on_done = {};
        // 结果已经取出，job 对象可以提前释放。
        record->job.reset();

        keep_record = record->want_result || static_cast<bool>(callback);
      }
    }

    if (!finished) {
      continue;
    }

    done_cv.notify_all();
    if (callback) {
      callback(id);
    }

    if (!keep_record) {
      std::lock_guard lock(mutex);
      const auto found = records.find(id);
      if (found != records.end() &&
          found->second->state == JobState::Finished) {
        records.erase(found);
      }
    }
  }
}

JobWorker::JobWorker(JobContext context, std::size_t thread_count)
    : impl_(std::make_unique<Impl>(*this, std::move(context))) {
  if (thread_count == 0) {
    thread_count = 1;
  }

  impl_->threads.reserve(thread_count);
  for (std::size_t index = 0; index < thread_count; ++index) {
    impl_->threads.emplace_back([this] { impl_->run(); });
  }
}

JobWorker::~JobWorker() {
  shutdown();
}

void JobWorker::set_scan_worker(JobWorker &worker) {
  impl_->context.scan_worker = std::ref(worker);
}

JobId JobWorker::submit(std::unique_ptr<Job> job, bool want_result,
                        JobDoneCallback on_done) {
  if (!job) {
    return 0;
  }

  std::lock_guard lock(impl_->mutex);
  if (impl_->stopping) {
    return 0;
  }

  const JobId id = impl_->next_id++;
  auto record = std::make_shared<Record>();
  record->job = std::move(job);
  record->want_result = want_result;
  record->on_done = std::move(on_done);

  impl_->records.emplace(id, std::move(record));
  impl_->ready.push_back(id);
  impl_->work_cv.notify_one();
  return id;
}

void JobWorker::wake(JobId id) {
  std::lock_guard lock(impl_->mutex);
  const auto found = impl_->records.find(id);
  if (found == impl_->records.end() ||
      found->second->state != JobState::Waiting) {
    return;
  }

  found->second->state = JobState::Queued;
  impl_->ready.push_back(id);
  impl_->work_cv.notify_one();
}

JobSnapshot JobWorker::peek(JobId id) const {
  std::lock_guard lock(impl_->mutex);
  const auto found = impl_->records.find(id);
  if (found == impl_->records.end()) {
    return {};
  }

  JobSnapshot snapshot;
  snapshot.state = found->second->state;
  if (snapshot.state == JobState::Finished && found->second->outcome &&
      !found->second->outcome->has_value()) {
    snapshot.error = found->second->outcome->error();
  }
  return snapshot;
}

Result<JobOutcome, JobError> JobWorker::take_result(JobId id) {
  std::lock_guard lock(impl_->mutex);
  const auto found = impl_->records.find(id);
  if (found == impl_->records.end() ||
      found->second->state != JobState::Finished || !found->second->outcome) {
    return JobError::JobNotFinished;
  }

  auto result = std::move(*found->second->outcome);
  impl_->records.erase(found);
  return result;
}

Result<JobOutcome, JobError> JobWorker::wait(JobId id) {
  {
    std::unique_lock lock(impl_->mutex);
    impl_->done_cv.wait(lock, [this, id] {
      const auto found = impl_->records.find(id);
      return found == impl_->records.end() ||
             found->second->state == JobState::Finished;
    });
  }

  return take_result(id);
}

void JobWorker::shutdown() {
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping) {
      return;
    }
    impl_->stopping = true;
  }

  impl_->work_cv.notify_all();
  for (std::thread &thread : impl_->threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  impl_->threads.clear();

  {
    std::lock_guard lock(impl_->mutex);
    impl_->ready.clear();
    // 未完成的 job 直接丢弃；调用方不应依赖 shutdown 后的回调。
    impl_->records.clear();
  }
  impl_->done_cv.notify_all();
}

} // namespace tagimg
