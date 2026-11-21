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

#include "ai/clip_embedder.hpp"
#include "ai/health.hpp"
#include "ai/tagger.hpp"
#include "common/result.hpp"
#include "common/storage.hpp"
#include "db/tagimg_repository.hpp"
#include "image/image_metadata.hpp"
#include "image/image_processor.hpp"

namespace tagimg {

class ImageCacheStore;
class StoredImageCacheObject;

enum class JobError {
  InvalidArgument,
  InvalidEntityId,
  EntityNotFound,
  NotImage,
  RepositoryFailed,
  DownloadFailed,
  FileError,
  ImageTooLarge,
  // AI 服务不可达，或 tagger / CLIP 模型尚未就绪。
  AiUnavailable,
  TaggingFailed,
  // 生成 Chinese-CLIP 向量失败；向量是入库前置条件。
  EmbeddingFailed,
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
  std::uint64_t bytes = 0;
  TaggingResult tags;
  // Chinese-CLIP 图像向量（768 维，已 L2 归一化）。
  std::vector<float> embedding;
  // 从图片本身读出的尺寸、帧数、真实格式与拍摄时间。
  ImageMetadata metadata;
  // 重扫时下载的源文件缓存写对象；事务成功后由调用方 commit。
  // 初次扫描没有 image_id，因此为空。
  std::shared_ptr<StoredImageCacheObject> source_cache;
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

// 重新扫描一个作品内全部图片的输入。
//
// 语义：先逐张重新扫描，全部成功后才写库；任一图片失败则整单不写，
// 作品的旧向量与旧标签保持原样。
struct RescanArtworkRequest {
  std::string artwork_id;
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
// 所有引用对象都必须比 worker 活得久。
struct JobContext {
  StorageConfig storage;
  std::reference_wrapper<const TagimgRepository> tagimg_repository;
  std::reference_wrapper<WdTagger> tagger;
  std::reference_wrapper<ClipEmbedder> embedder;
  std::reference_wrapper<const AiHealthClient> ai_health;
  std::filesystem::path work_dir;
  // 可为空；为空表示缓存未启用。
  std::shared_ptr<ImageCacheStore> image_cache;
  // 重扫后按当前配置构造变体 key，用于清理旧变体。
  ImageProcessingConfig image_processing;
};

// 一次可执行的工作。
//
// job 只保存业务参数，依赖由 worker 通过 JobContext 注入。step() 在
// worker 线程里执行；需要等待其他 job 时可以返回 Pending，worker 会
// 保存现场并在 wake() 后再次调度。AddJob 选择同步扫描，因此会占用
// 当前 worker 直到整批图片处理完。
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
