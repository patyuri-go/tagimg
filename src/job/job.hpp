#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ai/tagger.hpp"
#include "common/result.hpp"
#include "db/entity_repository.hpp"
#include "db/tagimg_repository.hpp"

namespace tagimg {

enum class JobError {
  InvalidArgument,
  InvalidEntityId,
  EntityNotFound,
  NotImage,
  RepositoryFailed,
  DownloadFailed,
  FileError,
  ImageTooLarge,
  TaggerUnavailable,
  TaggingFailed,
  DuplicateImage,
  UnknownTag,
  UnknownArtist,
  TagHierarchyMissing,
  WriteFailed,
  // Job 的 step() 抛出异常，或返回了类型不符的结果。
  InternalError,
  // 结果还没生成，或已经被取走。
  JobNotFinished,
};

[[nodiscard]] std::string_view to_string(JobError error) noexcept;

// 单个实体的扫描结果。
struct ScanResult {
  std::string entity_id;
  // 实际下载成功的存储节点名称。
  std::string storage_name;
  std::uint64_t bytes = 0;
  TaggingResult tags;
};

// AddJob 的完整输入；entity_ids 与 title 必填。
//
// description / published_at 为 std::nullopt 时写 SQL NULL。
// tag_ids / artist_ids 是已经存在的标签、艺术家 UUID。
struct AddRequest {
  std::vector<std::string> entity_ids;
  std::string title;
  std::optional<std::string> description;
  std::optional<std::string> published_at;
  std::vector<std::string> tag_ids;
  std::vector<std::string> artist_ids;
};

// 用已有图片新建一个作品的输入；image_ids 与 title 必填。
//
// 与 AddRequest 的区别是把实体 ID 换成图片 ID：图片行已经存在，
// 只新建 artwork_images 关联，不会重新插入 images。
// rescan 为 true 时重新扫描每张图片，并把扫描标签加入新作品；
// 为 false 时新作品的标签只有 tag_ids。
struct ImageAddRequest {
  std::vector<std::string> image_ids;
  std::string title;
  std::optional<std::string> description;
  std::optional<std::string> published_at;
  std::vector<std::string> tag_ids;
  std::vector<std::string> artist_ids;
  bool rescan = false;
};

struct ScanFailure {
  std::string entity_id;
  JobError error = JobError::TaggingFailed;
  // 图片模式下失败的图片 ID；实体模式为空。
  std::string image_id;
};

struct AddResult {
  // 实体模式下的原始请求。
  AddRequest request;
  // 图片模式下的原始请求；实体模式为空。
  std::optional<ImageAddRequest> image_request;
  std::vector<ScanResult> scans;
  std::vector<ScanFailure> failures;
  // 扫描失败或写库失败时为空。
  std::string artwork_id;
  std::vector<std::string> image_ids;
  // 实际写入作品的扫描标签数量，不含调用方传入的 tag_ids。
  std::size_t tag_count = 0;
};

using JobOutcome = std::variant<ScanResult, AddResult>;

// job 在 worker 里的索引。
using JobId = std::uint64_t;

// job 完成时在 worker 线程里回调；回调里可以 take_result()。
using JobDoneCallback = std::function<void(JobId)>;

// step() 的返回值：Pending 表示让出线程、保存现场，等 worker 再次唤醒。
enum class StepResult { Pending, Done };

class JobWorker;

// 一次 step 需要的运行期信息。
struct JobRun {
  JobId id = 0;
  // 拥有当前 job 的 worker，job 可以用它唤醒自己。
  JobWorker &worker;
};

// job 用到的外部依赖，由 worker 持有并注入。
//
// 所有引用对象都必须比 worker 活得久。scan_worker 只给 AddJob 使用，
// 通过 JobWorker::set_scan_worker() 注入。
struct JobContext {
  std::reference_wrapper<const EntityRepository> entity_repository;
  std::reference_wrapper<const TagimgRepository> tagimg_repository;
  std::reference_wrapper<WdTagger> tagger;
  std::filesystem::path work_dir;
  std::optional<std::reference_wrapper<JobWorker>> scan_worker;
};

// 一次可执行的工作。
//
// job 只保存业务参数，依赖由 worker 通过 JobContext 注入。step() 必须
// 非阻塞：需要等待其他 job 时立刻返回 Pending，worker 会保存现场。
class Job {
public:
  virtual ~Job() = default;

  Job(const Job &) = delete;
  Job &operator=(const Job &) = delete;

  [[nodiscard]] virtual StepResult step(JobContext &context,
                                        const JobRun &run) = 0;

  // 返回 Done 之后调用一次，取走最终结果。
  [[nodiscard]] virtual Result<JobOutcome, JobError> take_outcome() = 0;

protected:
  Job() = default;
};

enum class JobState { Unknown, Queued, Running, Waiting, Finished };

struct JobSnapshot {
  JobState state = JobState::Unknown;
  // 只在 Finished 且失败时有值。
  std::optional<JobError> error;
};

// 异步执行 job 的 worker。
//
// 每个 worker 持有自己的线程池（默认 1 个线程，按提交顺序执行）和一张
// job 索引表。step() 返回 Pending 的 job 会挂起，不占用线程；依赖完成
// 后调用 wake() 把它放回 ready 队列。
//
// 生命周期：submit() 之后 job 归 worker 所有；Done 后结果留在记录里，
// 由 take_result() 取走并删除记录。want_result 为 false 且没有回调时，
// job 完成即删除。
class JobWorker {
public:
  explicit JobWorker(JobContext context, std::size_t thread_count = 1);

  ~JobWorker();

  JobWorker(const JobWorker &) = delete;
  JobWorker &operator=(const JobWorker &) = delete;
  JobWorker(JobWorker &&) = delete;
  JobWorker &operator=(JobWorker &&) = delete;

  // 注入扫描 worker；必须在提交 AddJob 之前调用。
  void set_scan_worker(JobWorker &worker);

  // 提交 job，返回索引。worker 已停止时返回 0。
  [[nodiscard]] JobId submit(std::unique_ptr<Job> job, bool want_result = false,
                             JobDoneCallback on_done = {});

  // 把挂起（Waiting）的 job 放回 ready 队列；其他状态忽略。
  void wake(JobId id);

  // 非阻塞查看状态。
  [[nodiscard]] JobSnapshot peek(JobId id) const;

  // 取走已完成 job 的结果，并删除记录。未完成时返回 JobNotFinished。
  [[nodiscard]] Result<JobOutcome, JobError> take_result(JobId id);

  // 阻塞等待完成。只给命令行/测试使用；HTTP 层应使用 on_done 回调。
  [[nodiscard]] Result<JobOutcome, JobError> wait(JobId id);

  // 停止接收新 job，等待正在执行的 job 结束；未完成的 job 会被丢弃。
  void shutdown();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace tagimg
