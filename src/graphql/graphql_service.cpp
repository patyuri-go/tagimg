#include "graphql/graphql_service.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <list>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <drogon/utils/Utilities.h>
#include <nlohmann/json.hpp>

#include "common/uuid.hpp"
#include "common/validation.hpp"
#include "http/request_context.hpp"
#include "graphql/ArtistObject.h"
#include "graphql/ArtworkConnectionObject.h"
#include "graphql/ArtworkObject.h"
#include "graphql/DeleteArtworkResultObject.h"
#include "graphql/DeleteImageResultObject.h"
#include "graphql/ImageArtworkObject.h"
#include "graphql/ImageConnectionObject.h"
#include "graphql/ImageObject.h"
#include "graphql/MutationObject.h"
#include "graphql/OrphanImageConnectionObject.h"
#include "graphql/OrphanImageObject.h"
#include "graphql/QueryObject.h"
#include "graphql/TagMatchObject.h"
#include "graphql/TagObject.h"
#include "graphql/TagPathObject.h"
#include "graphql/TagTreeMatchObject.h"
#include "graphql/TagTreeNodeObject.h"
#include "graphql/TagimgSchema.h"
#include "graphqlservice/GraphQLParse.h"
#include "graphqlservice/JSONResponse.h"

namespace tagimg {
namespace {

namespace gql = graphql;

using gql::response::IdType;
using gql::response::ListType;
using gql::response::MapType;
using gql::response::Type;
using gql::response::Value;

[[noreturn]] void fail(const std::string &code, const std::string &message) {
  throw std::runtime_error(code + ": " + message);
}

// 每个 GraphQL 请求在 worker 内共享的取消状态。
class GraphQLRequestState final : public gql::service::RequestState {
public:
  explicit GraphQLRequestState(std::shared_ptr<RequestContext> context)
      : context(std::move(context)) {
  }

  std::shared_ptr<RequestContext> context;
};

// 从 resolver 参数里取回请求上下文；resolver 入口与阻塞调用前后用它检查
// 取消状态。取消时抛异常，由 cppgraphqlgen 转成 errors。
class RequestScope {
public:
  explicit RequestScope(gql::service::FieldParams &&params) {
    if (params.state) {
      if (auto state =
              std::dynamic_pointer_cast<GraphQLRequestState>(params.state)) {
        context_ = std::move(state->context);
      }
    }
    check();
  }

  void check() const {
    if (!context_) {
      return;
    }
    if (!context_->cancelled()) {
      return;
    }
    switch (context_->reason()) {
    case CancelReason::None:
      return;
    case CancelReason::Timeout:
      fail("TIMEOUT", "request timed out");
    case CancelReason::ClientClosed:
    case CancelReason::Shutdown:
    case CancelReason::Explicit:
      fail("CANCELLED", "request cancelled");
    }
  }

private:
  std::shared_ptr<RequestContext> context_;
};

// 单个查询的 90 秒看门狗：到期取消上下文，resolver 在下一个检查点退出。
class DeadlineGuard {
public:
  DeadlineGuard(std::chrono::steady_clock::duration timeout,
                std::function<void()> on_deadline)
      : on_deadline_(std::move(on_deadline)) {
    thread_ = std::thread([this, timeout] {
      std::unique_lock lock(mutex_);
      if (!cv_.wait_for(lock, timeout, [this] { return done_; })) {
        lock.unlock();
        if (on_deadline_) {
          on_deadline_();
        }
      }
    });
  }

  ~DeadlineGuard() {
    {
      std::lock_guard lock(mutex_);
      done_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  DeadlineGuard(const DeadlineGuard &) = delete;
  DeadlineGuard &operator=(const DeadlineGuard &) = delete;

private:
  std::function<void()> on_deadline_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool done_ = false;
  std::thread thread_;
};

// 简单的按行日志，串行输出避免多 worker 线程交错。
void log_line(std::string line) {
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  std::cout << line << std::endl;
}

// add 任务结束时的统一日志；实体模式与图片模式共用。
void log_add_outcome(const std::string &task_id,
                     const Result<JobOutcome, JobError> &result) {
  if (!result) {
    log_line("[scan] task " + task_id +
             " failed: " + std::string(to_string(result.error())));
    return;
  }
  const auto *add = std::get_if<AddResult>(&*result);
  if (add == nullptr) {
    log_line("[scan] task " + task_id + " finished with an unexpected outcome");
    return;
  }
  if (!add->failures.empty()) {
    for (const ScanFailure &failure : add->failures) {
      // 图片模式下 entity_id 可能为空，用 image_id 指代失败对象。
      const std::string target = failure.image_id.empty()
                                     ? "entity " + failure.entity_id
                                     : "image " + failure.image_id;
      log_line("[scan] task " + task_id + " failed for " + target + ": " +
               std::string(to_string(failure.error)));
    }
    return;
  }
  log_line("[scan] task " + task_id + " finished: artwork " + add->artwork_id +
           ", " + std::to_string(add->image_ids.size()) + " image(s), " +
           std::to_string(add->tag_count) + " tag(s)");
}

std::string id_text(const IdType &id) {
  // 形如 "nope" 的短字符串会被 GraphQL ID 解析成 base64 字面量，这里统一
  // 按「不是 uuid 字符串」报错，避免把实现细节暴露给调用方。
  if (id.isBase64()) {
    fail("INVALID_ARGUMENT", "id must be a uuid string");
  }
  return id.get<IdType::OpaqueString>();
}

std::vector<std::string> id_list(const std::vector<IdType> &ids) {
  std::vector<std::string> output;
  output.reserve(ids.size());
  for (const IdType &id : ids) {
    output.push_back(id_text(id));
  }
  return output;
}

// UUID 文本比较忽略大小写；默认排除 ID 来自数据库，前端可能传大写。
bool same_uuid(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (std::tolower(static_cast<unsigned char>(left[index])) !=
        std::tolower(static_cast<unsigned char>(right[index]))) {
      return false;
    }
  }
  return true;
}

AddRequest to_add_request(gql::tagimg::AddArtworkInput input) {
  AddRequest request;
  request.entity_ids = id_list(input.entityIds);
  if (const auto error = validate_entity_ids(request.entity_ids)) {
    fail("INVALID_ARGUMENT", *error);
  }

  request.title = std::move(input.title);
  if (const auto error = validate_title(request.title)) {
    fail("INVALID_ARGUMENT", *error);
  }

  request.description = std::move(input.description);
  if (input.publishedAt) {
    if (const auto error = validate_published_at(*input.publishedAt)) {
      fail("INVALID_ARGUMENT", *error);
    }
    request.published_at = std::move(*input.publishedAt);
  }

  if (input.tagIds) {
    request.tag_ids = id_list(*input.tagIds);
  }
  if (const auto error = validate_uuid_list(request.tag_ids, "tagIds")) {
    fail("INVALID_ARGUMENT", *error);
  }

  if (input.artistIds) {
    request.artist_ids = id_list(*input.artistIds);
  }
  if (const auto error = validate_uuid_list(request.artist_ids, "artistIds")) {
    fail("INVALID_ARGUMENT", *error);
  }
  return request;
}

ImageAddRequest
to_image_add_request(gql::tagimg::AddArtworkFromImagesInput input) {
  ImageAddRequest request;
  request.image_ids = id_list(input.imageIds);
  if (const auto error = validate_image_ids(request.image_ids)) {
    fail("INVALID_ARGUMENT", *error);
  }

  request.title = std::move(input.title);
  if (const auto error = validate_title(request.title)) {
    fail("INVALID_ARGUMENT", *error);
  }

  request.description = std::move(input.description);
  if (input.publishedAt) {
    if (const auto error = validate_published_at(*input.publishedAt)) {
      fail("INVALID_ARGUMENT", *error);
    }
    request.published_at = std::move(*input.publishedAt);
  }

  if (input.tagIds) {
    request.tag_ids = id_list(*input.tagIds);
  }
  if (const auto error = validate_uuid_list(request.tag_ids, "tagIds")) {
    fail("INVALID_ARGUMENT", *error);
  }

  if (input.artistIds) {
    request.artist_ids = id_list(*input.artistIds);
  }
  if (const auto error = validate_uuid_list(request.artist_ids, "artistIds")) {
    fail("INVALID_ARGUMENT", *error);
  }

  request.rescan = input.rescan.value_or(false);
  return request;
}

// 查询结果到 GraphQL 对象的适配。
gql::service::AwaitableScalar<IdType> id_scalar(const std::string &id) {
  return gql::service::AwaitableScalar<IdType>(
      IdType(IdType::OpaqueString(id)));
}

std::vector<IdType> to_id_list(const std::vector<std::string> &ids) {
  std::vector<IdType> output;
  output.reserve(ids.size());
  for (const std::string &id : ids) {
    output.push_back(IdType(IdType::OpaqueString(id)));
  }
  return output;
}

class TagModel {
public:
  explicit TagModel(ArtworkTagInfo info) : info_(std::move(info)) {
  }

  gql::service::AwaitableScalar<IdType> getId() const {
    return id_scalar(info_.id);
  }

  gql::service::AwaitableScalar<std::string> getName() const {
    return gql::service::AwaitableScalar<std::string>(info_.name);
  }

  gql::service::AwaitableScalar<std::optional<std::string>>
  getCategory() const {
    return gql::service::AwaitableScalar<std::optional<std::string>>(
        info_.category);
  }

  gql::service::AwaitableScalar<std::vector<std::string>> getAliases() const {
    return gql::service::AwaitableScalar<std::vector<std::string>>(
        info_.aliases);
  }

  gql::service::AwaitableScalar<std::optional<IdType>> getParentId() const {
    std::optional<IdType> parent_id;
    if (info_.parent_id) {
      parent_id = IdType(IdType::OpaqueString(*info_.parent_id));
    }
    return gql::service::AwaitableScalar<std::optional<IdType>>(
        std::move(parent_id));
  }

private:
  ArtworkTagInfo info_;
};

class TagPathModel {
public:
  TagPathModel(std::string id, std::string path)
      : id_(std::move(id)), path_(std::move(path)) {
  }

  gql::service::AwaitableScalar<IdType> getId() const {
    return id_scalar(id_);
  }

  gql::service::AwaitableScalar<std::string> getPath() const {
    return gql::service::AwaitableScalar<std::string>(path_);
  }

private:
  std::string id_;
  std::string path_;
};

class TagMatchModel {
public:
  explicit TagMatchModel(TagMatch info) : info_(std::move(info)) {
  }

  gql::service::AwaitableScalar<IdType> getTagId() const {
    return id_scalar(info_.id);
  }

  gql::service::AwaitableScalar<std::optional<std::string>> getName() const {
    return gql::service::AwaitableScalar<std::optional<std::string>>(
        info_.name);
  }

  gql::service::AwaitableScalar<std::optional<std::string>> getTagPath() const {
    return gql::service::AwaitableScalar<std::optional<std::string>>(
        info_.path);
  }

private:
  TagMatch info_;
};

class TagTreeNodeModel {
public:
  explicit TagTreeNodeModel(ArtworkTagInfo info) : info_(std::move(info)) {
  }

  gql::service::AwaitableScalar<IdType> getId() const {
    return id_scalar(info_.id);
  }

  gql::service::AwaitableScalar<std::optional<IdType>> getParentId() const {
    std::optional<IdType> parent_id;
    if (info_.parent_id) {
      parent_id = IdType(IdType::OpaqueString(*info_.parent_id));
    }
    return gql::service::AwaitableScalar<std::optional<IdType>>(
        std::move(parent_id));
  }

  gql::service::AwaitableScalar<std::string> getName() const {
    return gql::service::AwaitableScalar<std::string>(info_.name);
  }

  gql::service::AwaitableScalar<std::vector<std::string>> getAliases() const {
    return gql::service::AwaitableScalar<std::vector<std::string>>(
        info_.aliases);
  }

  gql::service::AwaitableScalar<std::string> getPath() const {
    return gql::service::AwaitableScalar<std::string>(info_.path);
  }

  gql::service::AwaitableScalar<bool> getHasChildren() const {
    return gql::service::AwaitableScalar<bool>(info_.has_children);
  }

private:
  ArtworkTagInfo info_;
};

class TagTreeMatchModel {
public:
  explicit TagTreeMatchModel(TagTreeMatchInfo info) : info_(std::move(info)) {
  }

  gql::service::AwaitableObject<
      std::shared_ptr<gql::tagimg::object::TagTreeNode>>
  getNode() const {
    return gql::service::AwaitableObject<
        std::shared_ptr<gql::tagimg::object::TagTreeNode>>(
        std::make_shared<gql::tagimg::object::TagTreeNode>(
            std::make_shared<TagTreeNodeModel>(info_.node)));
  }

  gql::service::AwaitableObject<
      std::vector<std::shared_ptr<gql::tagimg::object::TagTreeNode>>>
  getAncestors() const {
    std::vector<std::shared_ptr<gql::tagimg::object::TagTreeNode>> items;
    items.reserve(info_.ancestors.size());
    for (const ArtworkTagInfo &ancestor : info_.ancestors) {
      items.push_back(std::make_shared<gql::tagimg::object::TagTreeNode>(
          std::make_shared<TagTreeNodeModel>(ancestor)));
    }
    return gql::service::AwaitableObject<
        std::vector<std::shared_ptr<gql::tagimg::object::TagTreeNode>>>(
        std::move(items));
  }

  gql::service::AwaitableScalar<std::string> getMatchedName() const {
    return gql::service::AwaitableScalar<std::string>(info_.matched_name);
  }

  gql::service::AwaitableScalar<bool> getMatchedAlias() const {
    return gql::service::AwaitableScalar<bool>(info_.matched_alias);
  }

private:
  TagTreeMatchInfo info_;
};

class ArtistModel {
public:
  explicit ArtistModel(ArtworkArtistInfo info) : info_(std::move(info)) {
  }

  gql::service::AwaitableScalar<IdType> getId() const {
    return id_scalar(info_.id);
  }

  gql::service::AwaitableScalar<std::string> getName() const {
    return gql::service::AwaitableScalar<std::string>(info_.name);
  }

private:
  ArtworkArtistInfo info_;
};

class ArtworkModel {
public:
  explicit ArtworkModel(ArtworkSummary item) : item_(std::move(item)) {
  }

  gql::service::AwaitableScalar<IdType> getId() const {
    return id_scalar(item_.id);
  }

  gql::service::AwaitableScalar<std::string> getTitle() const {
    return gql::service::AwaitableScalar<std::string>(item_.title);
  }

  gql::service::AwaitableScalar<std::optional<std::string>>
  getDescription() const {
    return gql::service::AwaitableScalar<std::optional<std::string>>(
        item_.description);
  }

  gql::service::AwaitableScalar<std::optional<std::string>>
  getPublishedAt() const {
    return gql::service::AwaitableScalar<std::optional<std::string>>(
        item_.published_at);
  }

  gql::service::AwaitableScalar<std::string> getCreatedAt() const {
    return gql::service::AwaitableScalar<std::string>(item_.created_at);
  }

  gql::service::AwaitableScalar<std::string> getUpdatedAt() const {
    return gql::service::AwaitableScalar<std::string>(item_.updated_at);
  }

  gql::service::AwaitableScalar<int> getImageCount() const {
    return gql::service::AwaitableScalar<int>(
        static_cast<int>(item_.image_ids.size()));
  }

  gql::service::AwaitableScalar<std::optional<double>> getSortScore() const {
    return gql::service::AwaitableScalar<std::optional<double>>(
        item_.sort_score);
  }

  gql::service::AwaitableScalar<std::vector<IdType>> getImageIds() const {
    return gql::service::AwaitableScalar<std::vector<IdType>>(
        to_id_list(item_.image_ids));
  }

  gql::service::AwaitableScalar<std::vector<IdType>> getTagIds() const {
    return gql::service::AwaitableScalar<std::vector<IdType>>(
        to_id_list(item_.tag_ids));
  }

  gql::service::AwaitableObject<
      std::vector<std::shared_ptr<gql::tagimg::object::Tag>>>
  getTags() const {
    std::vector<std::shared_ptr<gql::tagimg::object::Tag>> tags;
    tags.reserve(item_.tags.size());
    for (const ArtworkTagInfo &tag : item_.tags) {
      tags.push_back(std::make_shared<gql::tagimg::object::Tag>(
          std::make_shared<TagModel>(tag)));
    }
    return gql::service::AwaitableObject<
        std::vector<std::shared_ptr<gql::tagimg::object::Tag>>>(
        std::move(tags));
  }

  gql::service::AwaitableObject<
      std::vector<std::shared_ptr<gql::tagimg::object::TagPath>>>
  getTagPaths() const {
    std::vector<std::shared_ptr<gql::tagimg::object::TagPath>> paths;
    paths.reserve(item_.tags.size());
    for (const ArtworkTagInfo &tag : item_.tags) {
      paths.push_back(std::make_shared<gql::tagimg::object::TagPath>(
          std::make_shared<TagPathModel>(tag.id, tag.path)));
    }
    return gql::service::AwaitableObject<
        std::vector<std::shared_ptr<gql::tagimg::object::TagPath>>>(
        std::move(paths));
  }

  gql::service::AwaitableScalar<std::vector<IdType>> getArtistIds() const {
    return gql::service::AwaitableScalar<std::vector<IdType>>(
        to_id_list(item_.artist_ids));
  }

  gql::service::AwaitableObject<
      std::vector<std::shared_ptr<gql::tagimg::object::Artist>>>
  getArtists() const {
    std::vector<std::shared_ptr<gql::tagimg::object::Artist>> artists;
    artists.reserve(item_.artists.size());
    for (const ArtworkArtistInfo &artist : item_.artists) {
      artists.push_back(std::make_shared<gql::tagimg::object::Artist>(
          std::make_shared<ArtistModel>(artist)));
    }
    return gql::service::AwaitableObject<
        std::vector<std::shared_ptr<gql::tagimg::object::Artist>>>(
        std::move(artists));
  }

private:
  ArtworkSummary item_;
};

class ImageArtworkModel {
public:
  explicit ImageArtworkModel(ImageArtworkInfo info) : info_(std::move(info)) {
  }

  gql::service::AwaitableScalar<IdType> getId() const {
    return id_scalar(info_.id);
  }

  gql::service::AwaitableScalar<std::string> getTitle() const {
    return gql::service::AwaitableScalar<std::string>(info_.title);
  }

  gql::service::AwaitableScalar<std::optional<std::string>>
  getPublishedAt() const {
    return gql::service::AwaitableScalar<std::optional<std::string>>(
        info_.published_at);
  }

private:
  ImageArtworkInfo info_;
};

class ImageModel {
public:
  explicit ImageModel(ImageSummary item) : item_(std::move(item)) {
  }

  gql::service::AwaitableScalar<IdType> getId() const {
    return id_scalar(item_.id);
  }

  gql::service::AwaitableScalar<std::optional<int>> getWidth() const {
    return gql::service::AwaitableScalar<std::optional<int>>(item_.width);
  }

  gql::service::AwaitableScalar<std::optional<int>> getHeight() const {
    return gql::service::AwaitableScalar<std::optional<int>>(item_.height);
  }

  gql::service::AwaitableScalar<std::optional<int>> getFrameCount() const {
    return gql::service::AwaitableScalar<std::optional<int>>(item_.frame_count);
  }

  gql::service::AwaitableScalar<std::string> getCreatedAt() const {
    return gql::service::AwaitableScalar<std::string>(item_.created_at);
  }

  gql::service::AwaitableScalar<std::optional<std::string>> getScanAt() const {
    return gql::service::AwaitableScalar<std::optional<std::string>>(
        item_.scan_at);
  }

  gql::service::AwaitableScalar<std::optional<std::string>>
  getPublishedAt() const {
    return gql::service::AwaitableScalar<std::optional<std::string>>(
        item_.published_at);
  }

  gql::service::AwaitableScalar<std::vector<IdType>> getTagIds() const {
    return gql::service::AwaitableScalar<std::vector<IdType>>(
        to_id_list(item_.tag_ids));
  }

  gql::service::AwaitableObject<
      std::vector<std::shared_ptr<gql::tagimg::object::Tag>>>
  getTags() const {
    std::vector<std::shared_ptr<gql::tagimg::object::Tag>> tags;
    tags.reserve(item_.tags.size());
    for (const ArtworkTagInfo &tag : item_.tags) {
      tags.push_back(std::make_shared<gql::tagimg::object::Tag>(
          std::make_shared<TagModel>(tag)));
    }
    return gql::service::AwaitableObject<
        std::vector<std::shared_ptr<gql::tagimg::object::Tag>>>(
        std::move(tags));
  }

  gql::service::AwaitableObject<
      std::vector<std::shared_ptr<gql::tagimg::object::TagPath>>>
  getTagPaths() const {
    std::vector<std::shared_ptr<gql::tagimg::object::TagPath>> paths;
    paths.reserve(item_.tags.size());
    for (const ArtworkTagInfo &tag : item_.tags) {
      paths.push_back(std::make_shared<gql::tagimg::object::TagPath>(
          std::make_shared<TagPathModel>(tag.id, tag.path)));
    }
    return gql::service::AwaitableObject<
        std::vector<std::shared_ptr<gql::tagimg::object::TagPath>>>(
        std::move(paths));
  }

  gql::service::AwaitableObject<
      std::vector<std::shared_ptr<gql::tagimg::object::ImageArtwork>>>
  getArtworks() const {
    std::vector<std::shared_ptr<gql::tagimg::object::ImageArtwork>> artworks;
    artworks.reserve(item_.artworks.size());
    for (const ImageArtworkInfo &artwork : item_.artworks) {
      artworks.push_back(std::make_shared<gql::tagimg::object::ImageArtwork>(
          std::make_shared<ImageArtworkModel>(artwork)));
    }
    return gql::service::AwaitableObject<
        std::vector<std::shared_ptr<gql::tagimg::object::ImageArtwork>>>(
        std::move(artworks));
  }

  gql::service::AwaitableScalar<std::optional<double>> getSortScore() const {
    return gql::service::AwaitableScalar<std::optional<double>>(
        item_.sort_score);
  }

private:
  ImageSummary item_;
};

class ArtworkConnectionModel {
public:
  ArtworkConnectionModel(
      std::vector<std::shared_ptr<gql::tagimg::object::Artwork>> items,
      bool has_more, std::optional<int> total, int limit, int offset)
      : items_(std::move(items)), has_more_(has_more), total_(total),
        limit_(limit), offset_(offset) {
  }

  gql::service::AwaitableObject<
      std::vector<std::shared_ptr<gql::tagimg::object::Artwork>>>
  getItems() const {
    return gql::service::AwaitableObject<
        std::vector<std::shared_ptr<gql::tagimg::object::Artwork>>>(items_);
  }

  gql::service::AwaitableScalar<bool> getHasMore() const {
    return gql::service::AwaitableScalar<bool>(has_more_);
  }

  gql::service::AwaitableScalar<std::optional<int>> getTotal() const {
    return gql::service::AwaitableScalar<std::optional<int>>(total_);
  }

  gql::service::AwaitableScalar<int> getLimit() const {
    return gql::service::AwaitableScalar<int>(limit_);
  }

  gql::service::AwaitableScalar<int> getOffset() const {
    return gql::service::AwaitableScalar<int>(offset_);
  }

private:
  std::vector<std::shared_ptr<gql::tagimg::object::Artwork>> items_;
  bool has_more_;
  std::optional<int> total_;
  int limit_;
  int offset_;
};

class ImageConnectionModel {
public:
  ImageConnectionModel(
      std::vector<std::shared_ptr<gql::tagimg::object::Image>> items,
      bool has_more, std::optional<int> total, int limit, int offset)
      : items_(std::move(items)), has_more_(has_more), total_(total),
        limit_(limit), offset_(offset) {
  }

  gql::service::AwaitableObject<
      std::vector<std::shared_ptr<gql::tagimg::object::Image>>>
  getItems() const {
    return gql::service::AwaitableObject<
        std::vector<std::shared_ptr<gql::tagimg::object::Image>>>(items_);
  }

  gql::service::AwaitableScalar<bool> getHasMore() const {
    return gql::service::AwaitableScalar<bool>(has_more_);
  }

  gql::service::AwaitableScalar<std::optional<int>> getTotal() const {
    return gql::service::AwaitableScalar<std::optional<int>>(total_);
  }

  gql::service::AwaitableScalar<int> getLimit() const {
    return gql::service::AwaitableScalar<int>(limit_);
  }

  gql::service::AwaitableScalar<int> getOffset() const {
    return gql::service::AwaitableScalar<int>(offset_);
  }

private:
  std::vector<std::shared_ptr<gql::tagimg::object::Image>> items_;
  bool has_more_;
  std::optional<int> total_;
  int limit_;
  int offset_;
};

class OrphanImageModel {
public:
  explicit OrphanImageModel(OrphanImage info) : info_(std::move(info)) {
  }

  gql::service::AwaitableScalar<IdType> getId() const {
    return id_scalar(info_.id);
  }

  gql::service::AwaitableScalar<std::optional<int>> getWidth() const {
    return gql::service::AwaitableScalar<std::optional<int>>(info_.width);
  }

  gql::service::AwaitableScalar<std::optional<int>> getHeight() const {
    return gql::service::AwaitableScalar<std::optional<int>>(info_.height);
  }

  gql::service::AwaitableScalar<std::optional<int>> getFrameCount() const {
    return gql::service::AwaitableScalar<std::optional<int>>(info_.frame_count);
  }

  gql::service::AwaitableScalar<std::string> getCreatedAt() const {
    return gql::service::AwaitableScalar<std::string>(info_.created_at);
  }

  gql::service::AwaitableScalar<std::optional<std::string>> getScanAt() const {
    return gql::service::AwaitableScalar<std::optional<std::string>>(
        info_.scan_at);
  }

private:
  OrphanImage info_;
};

class OrphanImageConnectionModel {
public:
  OrphanImageConnectionModel(
      std::vector<std::shared_ptr<gql::tagimg::object::OrphanImage>> items,
      bool has_more, std::optional<int> total, int limit, int offset)
      : items_(std::move(items)), has_more_(has_more), total_(total),
        limit_(limit), offset_(offset) {
  }

  gql::service::AwaitableObject<
      std::vector<std::shared_ptr<gql::tagimg::object::OrphanImage>>>
  getItems() const {
    return gql::service::AwaitableObject<
        std::vector<std::shared_ptr<gql::tagimg::object::OrphanImage>>>(items_);
  }

  gql::service::AwaitableScalar<bool> getHasMore() const {
    return gql::service::AwaitableScalar<bool>(has_more_);
  }

  gql::service::AwaitableScalar<std::optional<int>> getTotal() const {
    return gql::service::AwaitableScalar<std::optional<int>>(total_);
  }

  gql::service::AwaitableScalar<int> getLimit() const {
    return gql::service::AwaitableScalar<int>(limit_);
  }

  gql::service::AwaitableScalar<int> getOffset() const {
    return gql::service::AwaitableScalar<int>(offset_);
  }

private:
  std::vector<std::shared_ptr<gql::tagimg::object::OrphanImage>> items_;
  bool has_more_;
  std::optional<int> total_;
  int limit_;
  int offset_;
};

class DeleteArtworkResultModel {
public:
  explicit DeleteArtworkResultModel(DeleteArtworkResult result)
      : result_(result) {
  }

  gql::service::AwaitableScalar<bool> getDeleted() const {
    return gql::service::AwaitableScalar<bool>(result_.deleted);
  }

  gql::service::AwaitableScalar<int> getNewlyOrphanedImageCount() const {
    return gql::service::AwaitableScalar<int>(
        static_cast<int>(result_.newly_orphaned_image_count));
  }

private:
  DeleteArtworkResult result_;
};

class DeleteImageResultModel {
public:
  explicit DeleteImageResultModel(DeleteImageResult result)
      : result_(result) {
  }

  gql::service::AwaitableScalar<int> getDeletedCount() const {
    return gql::service::AwaitableScalar<int>(
        static_cast<int>(result_.deleted_count));
  }

  gql::service::AwaitableScalar<std::vector<IdType>> getMissingIds() const {
    return gql::service::AwaitableScalar<std::vector<IdType>>(
        to_id_list(result_.missing_ids));
  }

private:
  DeleteImageResult result_;
};

[[noreturn]] void fail_repository(TagimgRepositoryError error) {
  switch (error) {
  case TagimgRepositoryError::InvalidArgument:
    fail("INVALID_ARGUMENT", "invalid argument");
  case TagimgRepositoryError::UnknownTag:
    fail("UNKNOWN_TAG", "tag id does not exist");
  case TagimgRepositoryError::UnknownArtist:
    fail("UNKNOWN_ARTIST", "artist id does not exist");
  case TagimgRepositoryError::UnknownImage:
    fail("UNKNOWN_IMAGE", "image id does not exist");
  case TagimgRepositoryError::TagNameConflict:
    fail("TAG_NAME_CONFLICT",
         "a tag with the same name already exists under this parent");
  case TagimgRepositoryError::TagParentNotFound:
    fail("TAG_PARENT_NOT_FOUND", "parent tag does not exist");
  case TagimgRepositoryError::TagCycle:
    fail("TAG_CYCLE",
         "tag cannot be moved under itself or its descendants");
  case TagimgRepositoryError::DuplicateImage:
  case TagimgRepositoryError::TagHierarchyMissing:
  case TagimgRepositoryError::ConnectionFailed:
  case TagimgRepositoryError::QueryFailed:
    break;
  }
  fail("QUERY_FAILED", "database query failed");
}

// Query / Mutation 的实现对象；生成代码按 concept 直接调用同名方法。
class Root {
public:
  Root(std::shared_ptr<ServiceRuntime> runtime,
       std::shared_ptr<const TagimgRepository> repository,
       std::shared_ptr<ClipEmbedder> clip_embedder,
       std::vector<std::string> default_excluded_tag_ids)
      : runtime_(std::move(runtime)), repository_(std::move(repository)),
        clip_embedder_(std::move(clip_embedder)),
        default_excluded_tag_ids_(std::move(default_excluded_tag_ids)) {
    if (!runtime_ || !repository_ || !clip_embedder_) {
      throw std::invalid_argument(
          "Root requires ServiceRuntime, repository and ClipEmbedder");
    }
  }

  gql::service::AwaitableScalar<std::string>
  getPing(gql::service::FieldParams &&params) const {
    RequestScope request(std::move(params));
    return gql::service::AwaitableScalar<std::string>(std::string("pong"));
  }

  gql::service::AwaitableScalar<IdType>
  applyAddArtwork(gql::tagimg::AddArtworkInput input) const {
    AddRequest request = to_add_request(std::move(input));
    const std::string task_id = drogon::utils::getUuid();
    const std::size_t entity_count = request.entity_ids.size();

    log_line("[scan] task " + task_id +
             " queued: " + std::to_string(entity_count) + " entities");

    const bool accepted =
        runtime_->submit(std::move(request),
                         [task_id](const Result<JobOutcome, JobError> &result) {
                           log_add_outcome(task_id, result);
                         });

    if (!accepted) {
      fail("SERVICE_UNAVAILABLE", "job worker is not running");
    }
    return gql::service::AwaitableScalar<IdType>(
        IdType(IdType::OpaqueString(task_id)));
  }

  gql::service::AwaitableScalar<IdType> applyAddArtworkFromImages(
      gql::tagimg::AddArtworkFromImagesInput input) const {
    ImageAddRequest request = to_image_add_request(std::move(input));
    const std::string task_id = drogon::utils::getUuid();
    const std::size_t image_count = request.image_ids.size();

    log_line("[scan] task " + task_id +
             " queued: " + std::to_string(image_count) +
             " image(s), rescan=" + (request.rescan ? "true" : "false"));

    const bool accepted =
        runtime_->submit(std::move(request),
                         [task_id](const Result<JobOutcome, JobError> &result) {
                           log_add_outcome(task_id, result);
                         });

    if (!accepted) {
      fail("SERVICE_UNAVAILABLE", "job worker is not running");
    }
    return gql::service::AwaitableScalar<IdType>(
        IdType(IdType::OpaqueString(task_id)));
  }

  gql::service::AwaitableObject<std::shared_ptr<gql::tagimg::object::Artwork>>
  getArtwork(gql::service::FieldParams &&params, IdType id) const {
    RequestScope request(std::move(params));
    const std::string text = id_text(id);
    if (!is_uuid(text)) {
      fail("INVALID_ARGUMENT", "id is not a valid uuid");
    }

    request.check();
    const auto result = repository_->find_artwork(text);
    if (!result) {
      fail_repository(result.error());
    }
    request.check();
    if (!result->has_value()) {
      return gql::service::AwaitableObject<
          std::shared_ptr<gql::tagimg::object::Artwork>>(
          std::shared_ptr<gql::tagimg::object::Artwork>{});
    }

    return gql::service::AwaitableObject<
        std::shared_ptr<gql::tagimg::object::Artwork>>(
        std::make_shared<gql::tagimg::object::Artwork>(
            std::make_shared<ArtworkModel>(std::move(**result))));
  }

  gql::service::AwaitableObject<
      std::shared_ptr<gql::tagimg::object::ArtworkConnection>>
  getArtworks(gql::service::FieldParams &&params,
              std::unique_ptr<gql::tagimg::ArtworkFilter> filter,
              std::optional<gql::tagimg::ArtworkOrderField> order_by,
              std::optional<gql::tagimg::SortDirection> direction,
              std::optional<int> limit, std::optional<int> offset,
              std::optional<bool> include_total,
              std::optional<std::string> sort_text) const {
    RequestScope request(std::move(params));
    ArtworkSearchQuery query;
    if (filter) {
      if (filter->title) {
        if (utf8_length(*filter->title) > kMaxTitleLength) {
          fail("INVALID_ARGUMENT", "title must be at most 500 characters");
        }
        if (!filter->title->empty()) {
          query.title = *filter->title;
        }
      }
      const auto read_ids = [](const std::optional<std::vector<IdType>> &ids,
                               const char *field) -> std::vector<std::string> {
        if (!ids) {
          return {};
        }
        std::vector<std::string> values = id_list(*ids);
        if (const auto error = validate_uuid_list(values, field)) {
          fail("INVALID_ARGUMENT", *error);
        }
        return values;
      };
      query.entity_ids = read_ids(filter->entityIds, "entityIds");
      query.artist_ids = read_ids(filter->artistIds, "artistIds");
      query.include_tag_ids = read_ids(filter->includeTagIds, "includeTagIds");
      query.exclude_tag_ids = read_ids(filter->excludeTagIds, "excludeTagIds");

      if (filter->includeMode) {
        query.include_all_tags =
            *filter->includeMode == gql::tagimg::TagMatchMode::ALL;
      }
      if (filter->expandIncludeTagDescendants) {
        query.expand_include_tag_descendants =
            *filter->expandIncludeTagDescendants;
      }
      if (filter->expandExcludeTagDescendants) {
        query.expand_exclude_tag_descendants =
            *filter->expandExcludeTagDescendants;
      }
    }

    if (query.entity_ids.empty()) {
      apply_default_exclusions(query.include_tag_ids, query.exclude_tag_ids);
    }

    const auto order_field =
        order_by.value_or(gql::tagimg::ArtworkOrderField::CREATED_AT);
    if (order_field == gql::tagimg::ArtworkOrderField::SIMILARITY) {
      if (!sort_text || sort_text->empty()) {
        fail("INVALID_ARGUMENT", "orderBy SIMILARITY requires sortText");
      }
      if (utf8_length(*sort_text) > 512) {
        fail("INVALID_ARGUMENT", "sortText must be at most 512 characters");
      }
      const bool blank =
          std::all_of(sort_text->begin(), sort_text->end(),
                      [](unsigned char byte) {
                        return std::isspace(byte) != 0;
                      });
      if (blank) {
        fail("INVALID_ARGUMENT", "sortText must not be blank");
      }
      // 这里同步调用 sidecar 编码文本，会阻塞当前线程直到返回。
      request.check();
      auto embedding = clip_embedder_->embed_text(*sort_text);
      request.check();
      if (!embedding) {
        fail("AI_UNAVAILABLE",
             std::string("failed to embed sortText: ") +
                 std::string(to_string(embedding.error())));
      }
      query.order_by = ArtworkSortField::Similarity;
      query.sort_embedding = std::move(embedding->values);
      // 相似度越高越靠前，方向固定为 DESC。
      query.descending = true;
    } else {
      if (sort_text) {
        fail("INVALID_ARGUMENT",
             "sortText is only supported with orderBy SIMILARITY");
      }
      switch (order_field) {
      case gql::tagimg::ArtworkOrderField::CREATED_AT:
        query.order_by = ArtworkSortField::CreatedAt;
        break;
      case gql::tagimg::ArtworkOrderField::PUBLISHED_AT:
        query.order_by = ArtworkSortField::PublishedAt;
        break;
      case gql::tagimg::ArtworkOrderField::TITLE:
        query.order_by = ArtworkSortField::Title;
        break;
      default:
        // SIMILARITY 已在上面处理，其余枚举值不存在。
        fail("INVALID_ARGUMENT", "unsupported orderBy");
      }
      query.descending =
          direction.value_or(gql::tagimg::SortDirection::DESC) ==
          gql::tagimg::SortDirection::DESC;
    }

    if (limit) {
      if (*limit < 1 || *limit > 100) {
        fail("INVALID_ARGUMENT", "limit must be between 1 and 100");
      }
      query.limit = static_cast<std::size_t>(*limit);
    }
    if (offset) {
      if (*offset < 0 || *offset > 10000) {
        fail("INVALID_ARGUMENT", "offset must be between 0 and 10000");
      }
      query.offset = static_cast<std::size_t>(*offset);
    }
    query.include_total = include_total.value_or(false);

    request.check();
    const auto result = repository_->search_artworks(query);
    if (!result) {
      fail_repository(result.error());
    }
    request.check();

    std::vector<std::shared_ptr<gql::tagimg::object::Artwork>> items;
    items.reserve(result->items.size());
    for (const ArtworkSummary &item : result->items) {
      items.push_back(std::make_shared<gql::tagimg::object::Artwork>(
          std::make_shared<ArtworkModel>(item)));
    }
    std::optional<int> total;
    if (result->total) {
      total = static_cast<int>(*result->total);
    }

    return gql::service::AwaitableObject<
        std::shared_ptr<gql::tagimg::object::ArtworkConnection>>(
        std::make_shared<gql::tagimg::object::ArtworkConnection>(
            std::make_shared<ArtworkConnectionModel>(
                std::move(items), result->has_more, total,
                static_cast<int>(query.limit),
                static_cast<int>(query.offset))));
  }

  gql::service::AwaitableObject<
      std::shared_ptr<gql::tagimg::object::ImageConnection>>
  getImages(gql::service::FieldParams &&params,
            std::unique_ptr<gql::tagimg::ImageFilter> filter,
            std::optional<gql::tagimg::ImageOrderField> order_by,
            std::optional<gql::tagimg::SortDirection> direction,
            std::optional<int> limit, std::optional<int> offset,
            std::optional<bool> include_total,
            std::optional<std::string> sort_text,
            std::optional<IdType> reference_image_id) const {
    RequestScope request(std::move(params));
    ImageSearchQuery query;
    if (filter) {
      const auto read_ids = [](const std::optional<std::vector<IdType>> &ids,
                               const char *field) -> std::vector<std::string> {
        if (!ids) {
          return {};
        }
        std::vector<std::string> values = id_list(*ids);
        if (const auto error = validate_uuid_list(values, field)) {
          fail("INVALID_ARGUMENT", *error);
        }
        return values;
      };
      query.entity_ids = read_ids(filter->entityIds, "entityIds");
      query.image_ids = read_ids(filter->imageIds, "imageIds");
      query.artist_ids = read_ids(filter->artistIds, "artistIds");
      query.include_tag_ids = read_ids(filter->includeTagIds, "includeTagIds");
      query.exclude_tag_ids = read_ids(filter->excludeTagIds, "excludeTagIds");

      if (filter->includeMode) {
        query.include_all_tags =
            *filter->includeMode == gql::tagimg::TagMatchMode::ALL;
      }
      if (filter->expandIncludeTagDescendants) {
        query.expand_include_tag_descendants =
            *filter->expandIncludeTagDescendants;
      }
      if (filter->expandExcludeTagDescendants) {
        query.expand_exclude_tag_descendants =
            *filter->expandExcludeTagDescendants;
      }
    }

    if (query.entity_ids.empty() && query.image_ids.empty()) {
      apply_default_exclusions(query.include_tag_ids, query.exclude_tag_ids);
    }

    std::optional<std::string> reference_id;
    if (reference_image_id) {
      reference_id = id_text(*reference_image_id);
      if (!is_uuid(*reference_id)) {
        fail("INVALID_ARGUMENT", "referenceImageId must be a uuid string");
      }
    }

    const auto order_field =
        order_by.value_or(gql::tagimg::ImageOrderField::CREATED_AT);
    if (order_field == gql::tagimg::ImageOrderField::SIMILARITY) {
      const bool has_text = sort_text && !sort_text->empty();
      if (has_text == reference_id.has_value()) {
        fail("INVALID_ARGUMENT",
             "orderBy SIMILARITY requires exactly one of sortText or "
             "referenceImageId");
      }
      if (reference_id) {
        request.check();
        const auto embedding =
            repository_->find_clip_embedding(*reference_id);
        request.check();
        if (!embedding) {
          fail_repository(embedding.error());
        }
        if (!embedding->has_value()) {
          fail("INVALID_ARGUMENT",
               "referenceImageId not found or has no embedding");
        }
        query.order_by = ImageSortField::Similarity;
        query.sort_embedding = std::move(**embedding);
        query.reference_image_id = *reference_id;
        query.descending = true;
      } else {
        if (utf8_length(*sort_text) > 512) {
          fail("INVALID_ARGUMENT",
               "sortText must be at most 512 characters");
        }
        const bool blank =
            std::all_of(sort_text->begin(), sort_text->end(),
                        [](unsigned char byte) {
                          return std::isspace(byte) != 0;
                        });
        if (blank) {
          fail("INVALID_ARGUMENT", "sortText must not be blank");
        }
        request.check();
        auto embedding = clip_embedder_->embed_text(*sort_text);
        request.check();
        if (!embedding) {
          fail("SERVICE_UNAVAILABLE",
               std::string("failed to embed sortText: ") +
                   std::string(to_string(embedding.error())));
        }
        query.order_by = ImageSortField::Similarity;
        query.sort_embedding = std::move(embedding->values);
        query.descending = true;
      }
    } else {
      if (sort_text) {
        fail("INVALID_ARGUMENT",
             "sortText is only supported with orderBy SIMILARITY");
      }
      if (reference_id) {
        fail("INVALID_ARGUMENT",
             "referenceImageId is only supported with orderBy SIMILARITY");
      }
      switch (order_field) {
      case gql::tagimg::ImageOrderField::CREATED_AT:
        query.order_by = ImageSortField::CreatedAt;
        break;
      case gql::tagimg::ImageOrderField::PUBLISHED_AT:
        query.order_by = ImageSortField::PublishedAt;
        break;
      case gql::tagimg::ImageOrderField::SCAN_AT:
        query.order_by = ImageSortField::ScanAt;
        break;
      default:
        fail("INVALID_ARGUMENT", "unsupported orderBy");
      }
      query.descending =
          direction.value_or(gql::tagimg::SortDirection::DESC) ==
          gql::tagimg::SortDirection::DESC;
    }

    if (limit) {
      if (*limit < 1 || *limit > 100) {
        fail("INVALID_ARGUMENT", "limit must be between 1 and 100");
      }
      query.limit = static_cast<std::size_t>(*limit);
    }
    if (offset) {
      if (*offset < 0 || *offset > 10000) {
        fail("INVALID_ARGUMENT", "offset must be between 0 and 10000");
      }
      query.offset = static_cast<std::size_t>(*offset);
    }
    query.include_total = include_total.value_or(false);

    request.check();
    const auto result = repository_->search_images(query);
    if (!result) {
      fail_repository(result.error());
    }
    request.check();

    std::vector<std::shared_ptr<gql::tagimg::object::Image>> items;
    items.reserve(result->items.size());
    for (const ImageSummary &item : result->items) {
      items.push_back(std::make_shared<gql::tagimg::object::Image>(
          std::make_shared<ImageModel>(item)));
    }
    std::optional<int> total;
    if (result->total) {
      total = static_cast<int>(*result->total);
    }

    return gql::service::AwaitableObject<
        std::shared_ptr<gql::tagimg::object::ImageConnection>>(
        std::make_shared<gql::tagimg::object::ImageConnection>(
            std::make_shared<ImageConnectionModel>(
                std::move(items), result->has_more, total,
                static_cast<int>(query.limit),
                static_cast<int>(query.offset))));
  }

  gql::service::AwaitableObject<
      std::vector<std::shared_ptr<gql::tagimg::object::TagMatch>>>
  getTags(gql::service::FieldParams &&params, std::string query,
          std::unique_ptr<gql::tagimg::TagSearchScope> scope,
          std::optional<int> limit) const {
    RequestScope request(std::move(params));
    TagSearchQuery search;
    search.text = std::move(query);
    if (search.text.empty() || utf8_length(search.text) > kMaxTagSearchLength) {
      fail("INVALID_ARGUMENT", "query must be 1 to 100 characters");
    }
    if (limit) {
      if (*limit < 1 || *limit > static_cast<int>(kMaxTagSearchLimit)) {
        fail("INVALID_ARGUMENT", "limit must be between 1 and 50");
      }
      search.limit = static_cast<std::size_t>(*limit);
    }
    if (scope) {
      if (scope->parentId) {
        const std::string parent = id_text(*scope->parentId);
        if (!is_uuid(parent)) {
          fail("INVALID_ARGUMENT", "parentId is not a valid uuid");
        }
        search.scope = TagSearchScope::Subtree;
        search.parent_id = parent;
      } else {
        // scope 传了对象但 parentId 为 null：只查全局根的直接子标签。
        search.scope = TagSearchScope::Root;
      }
      search.direct_only = scope->directOnly.value_or(false);
    }

    const auto result = repository_->search_tags(search);
    if (!result) {
      fail_repository(result.error());
    }
    request.check();

    std::vector<std::shared_ptr<gql::tagimg::object::TagMatch>> items;
    items.reserve(result->size());
    for (const TagMatch &match : *result) {
      items.push_back(std::make_shared<gql::tagimg::object::TagMatch>(
          std::make_shared<TagMatchModel>(match)));
    }
    return gql::service::AwaitableObject<
        std::vector<std::shared_ptr<gql::tagimg::object::TagMatch>>>(
        std::move(items));
  }

  gql::service::AwaitableObject<
      std::vector<std::shared_ptr<gql::tagimg::object::TagTreeNode>>>
  getTagChildren(gql::service::FieldParams &&params,
                 std::optional<IdType> parent_id) const {
    RequestScope request(std::move(params));

    std::optional<std::string> parent;
    if (parent_id) {
      std::string text = id_text(*parent_id);
      if (!is_uuid(text)) {
        fail("INVALID_ARGUMENT", "parentId is not a valid uuid");
      }
      parent = std::move(text);
    }

    const auto result = repository_->list_tag_children(parent);
    if (!result) {
      fail_repository(result.error());
    }
    request.check();

    std::vector<std::shared_ptr<gql::tagimg::object::TagTreeNode>> items;
    items.reserve(result->size());
    for (const ArtworkTagInfo &tag : *result) {
      items.push_back(std::make_shared<gql::tagimg::object::TagTreeNode>(
          std::make_shared<TagTreeNodeModel>(tag)));
    }
    return gql::service::AwaitableObject<
        std::vector<std::shared_ptr<gql::tagimg::object::TagTreeNode>>>(
        std::move(items));
  }

  gql::service::AwaitableObject<
      std::vector<std::shared_ptr<gql::tagimg::object::TagTreeMatch>>>
  getTagTreeSearch(gql::service::FieldParams &&params,
                   std::string query) const {
    RequestScope request(std::move(params));
    if (query.empty() || utf8_length(query) > kMaxTagSearchLength) {
      fail("INVALID_ARGUMENT", "query must be 1 to 100 characters");
    }

    const auto result = repository_->search_tag_tree(query);
    if (!result) {
      fail_repository(result.error());
    }
    request.check();

    std::vector<std::shared_ptr<gql::tagimg::object::TagTreeMatch>> items;
    items.reserve(result->size());
    for (const TagTreeMatchInfo &match : *result) {
      items.push_back(std::make_shared<gql::tagimg::object::TagTreeMatch>(
          std::make_shared<TagTreeMatchModel>(match)));
    }
    return gql::service::AwaitableObject<
        std::vector<std::shared_ptr<gql::tagimg::object::TagTreeMatch>>>(
        std::move(items));
  }

  gql::service::AwaitableObject<
      std::shared_ptr<gql::tagimg::object::OrphanImageConnection>>
  getOrphanImages(gql::service::FieldParams &&params,
                  std::optional<int> limit, std::optional<int> offset,
                  std::optional<bool> include_total) const {
    RequestScope request(std::move(params));
    OrphanImageQuery query;
    if (limit) {
      if (*limit < 1 || *limit > 100) {
        fail("INVALID_ARGUMENT", "limit must be between 1 and 100");
      }
      query.limit = static_cast<std::size_t>(*limit);
    }
    if (offset) {
      if (*offset < 0 || *offset > 10000) {
        fail("INVALID_ARGUMENT", "offset must be between 0 and 10000");
      }
      query.offset = static_cast<std::size_t>(*offset);
    }
    query.include_total = include_total.value_or(false);

    const auto result = repository_->list_orphan_images(query);
    if (!result) {
      fail_repository(result.error());
    }
    request.check();

    std::vector<std::shared_ptr<gql::tagimg::object::OrphanImage>> items;
    items.reserve(result->items.size());
    for (const OrphanImage &image : result->items) {
      items.push_back(std::make_shared<gql::tagimg::object::OrphanImage>(
          std::make_shared<OrphanImageModel>(image)));
    }
    std::optional<int> total;
    if (result->total) {
      total = static_cast<int>(*result->total);
    }

    return gql::service::AwaitableObject<
        std::shared_ptr<gql::tagimg::object::OrphanImageConnection>>(
        std::make_shared<gql::tagimg::object::OrphanImageConnection>(
            std::make_shared<OrphanImageConnectionModel>(
                std::move(items), result->has_more, total,
                static_cast<int>(query.limit),
                static_cast<int>(query.offset))));
  }

  gql::service::AwaitableObject<
      std::shared_ptr<gql::tagimg::object::DeleteArtworkResult>>
  applyDeleteArtwork(std::vector<IdType> ids) const {
    if (ids.empty()) {
      fail("INVALID_ARGUMENT", "ids must not be empty");
    }

    // 校验全部 ID 后再调用 repository，非法 UUID 不会删掉任何作品。
    // 忽略大小写去重，保留首次出现的文本形式。
    std::vector<std::string> text_ids;
    text_ids.reserve(ids.size());
    std::set<std::string> seen;
    for (const IdType &id : ids) {
      std::string text = id_text(id);
      if (!is_uuid(text)) {
        fail("INVALID_ARGUMENT", "ids must contain uuid strings");
      }
      std::string key = text;
      std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
      });
      if (seen.insert(key).second) {
        text_ids.push_back(std::move(text));
      }
    }

    const auto result = repository_->delete_artworks(text_ids);
    if (!result) {
      fail_repository(result.error());
    }

    return gql::service::AwaitableObject<
        std::shared_ptr<gql::tagimg::object::DeleteArtworkResult>>(
        std::make_shared<gql::tagimg::object::DeleteArtworkResult>(
            std::make_shared<DeleteArtworkResultModel>(*result)));
  }

  gql::service::AwaitableObject<
      std::shared_ptr<gql::tagimg::object::DeleteImageResult>>
  applyDeleteImages(std::vector<IdType> ids) const {
    if (ids.empty()) {
      fail("INVALID_ARGUMENT", "ids must not be empty");
    }

    // 校验全部 ID 后再调用 repository，非法 UUID 不会删掉任何图片。
    // 忽略大小写去重，保留首次出现的文本形式。
    std::vector<std::string> text_ids;
    text_ids.reserve(ids.size());
    std::set<std::string> seen;
    for (const IdType &id : ids) {
      std::string text = id_text(id);
      if (!is_uuid(text)) {
        fail("INVALID_ARGUMENT", "ids must contain uuid strings");
      }
      std::string key = text;
      std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
      });
      if (seen.insert(key).second) {
        text_ids.push_back(std::move(text));
      }
    }

    const auto result = repository_->delete_images(text_ids);
    if (!result) {
      fail_repository(result.error());
    }

    return gql::service::AwaitableObject<
        std::shared_ptr<gql::tagimg::object::DeleteImageResult>>(
        std::make_shared<gql::tagimg::object::DeleteImageResult>(
            std::make_shared<DeleteImageResultModel>(*result)));
  }

  gql::service::AwaitableScalar<IdType>
  applyUpdateArtwork(IdType id, gql::tagimg::UpdateArtworkInput input) const {
    const std::string text = id_text(id);
    if (!is_uuid(text)) {
      fail("INVALID_ARGUMENT", "id must be a uuid string");
    }

    bool has_change = false;

    if (input.title) {
      if (const auto error = validate_title(*input.title)) {
        fail("INVALID_ARGUMENT", *error);
      }
      has_change = true;
    }

    if (input.publishedAt) {
      if (const auto error = validate_published_at(*input.publishedAt)) {
        fail("INVALID_ARGUMENT", *error);
      }
      has_change = true;
    }

    const bool clear_published_at = input.clearPublishedAt.value_or(false);
    if (clear_published_at) {
      has_change = true;
    }
    if (clear_published_at && input.publishedAt) {
      fail("INVALID_ARGUMENT",
           "publishedAt and clearPublishedAt cannot be used together");
    }

    std::vector<std::string> add_tag_ids;
    if (input.addTagIds) {
      add_tag_ids = id_list(*input.addTagIds);
      if (const auto error =
              validate_tag_update_ids(add_tag_ids, "addTagIds")) {
        fail("INVALID_ARGUMENT", *error);
      }
      has_change = has_change || !add_tag_ids.empty();
    }

    std::vector<std::string> remove_tag_ids;
    if (input.removeTagIds) {
      remove_tag_ids = id_list(*input.removeTagIds);
      if (const auto error =
              validate_tag_update_ids(remove_tag_ids, "removeTagIds")) {
        fail("INVALID_ARGUMENT", *error);
      }
      has_change = has_change || !remove_tag_ids.empty();
    }

    std::vector<std::string> image_ids;
    if (input.imageIds) {
      image_ids = id_list(*input.imageIds);
      if (const auto error = validate_artwork_image_order(image_ids)) {
        fail("INVALID_ARGUMENT", *error);
      }
      has_change = true;
    }

    if (add_tag_ids.size() + remove_tag_ids.size() > kMaxArtworkEditTags) {
      fail("INVALID_ARGUMENT", "tag changes must contain at most " +
                                   std::to_string(kMaxArtworkEditTags) +
                                   " items");
    }

    // 同一个标签不能既添加又移除，否则结果取决于执行顺序。
    const std::set<std::string> add_tags(add_tag_ids.begin(),
                                         add_tag_ids.end());
    for (const std::string &tag_id : remove_tag_ids) {
      if (add_tags.count(tag_id) > 0) {
        fail("INVALID_ARGUMENT", "addTagIds and removeTagIds must not overlap");
      }
    }

    if (!has_change) {
      fail("INVALID_ARGUMENT", "at least one change is required");
    }

    // 参数全部校验通过后才开事务：事务对象持有连接租约与作品行锁。
    auto opened = repository_->begin_artwork_update(text);
    if (!opened) {
      fail_repository(opened.error());
    }
    if (!opened->has_value()) {
      fail("NOT_FOUND", "artwork not found");
    }
    ArtworkUpdateTransaction &txn = **opened;

    const auto apply = [](Result<void, TagimgRepositoryError> result) {
      if (!result) {
        fail_repository(result.error());
      }
    };

    if (input.title) {
      apply(txn.set_title(std::move(*input.title)));
    }
    if (input.publishedAt) {
      apply(txn.set_published_at(std::move(*input.publishedAt)));
    }
    if (clear_published_at) {
      apply(txn.clear_published_at());
    }
    if (!add_tag_ids.empty()) {
      apply(txn.add_tag_ids(add_tag_ids));
    }
    if (!remove_tag_ids.empty()) {
      apply(txn.remove_tag_ids(remove_tag_ids));
    }
    if (input.imageIds) {
      apply(txn.reorder_images(image_ids));
    }
    apply(txn.commit());

    return id_scalar(text);
  }

  gql::service::AwaitableObject<std::shared_ptr<gql::tagimg::object::Tag>>
  applyCreateTag(gql::tagimg::CreateTagInput input) const {
    TagCreateInput request;
    request.name = std::move(input.name);
    if (input.aliases) {
      request.aliases = std::move(*input.aliases);
    }
    if (input.parentId) {
      request.parent_id = id_text(*input.parentId);
    }

    const auto result = repository_->create_tag(request);
    if (!result) {
      fail_repository(result.error());
    }

    return gql::service::AwaitableObject<
        std::shared_ptr<gql::tagimg::object::Tag>>(
        std::make_shared<gql::tagimg::object::Tag>(
            std::make_shared<TagModel>(*result)));
  }

  gql::service::AwaitableObject<std::shared_ptr<gql::tagimg::object::Tag>>
  applyUpdateTag(IdType id, gql::tagimg::UpdateTagInput input) const {
    const std::string tag_id = id_text(id);
    if (!is_uuid(tag_id)) {
      fail("INVALID_ARGUMENT", "id must be a uuid string");
    }

    TagUpdateInput request;
    if (input.name) {
      request.name = std::move(*input.name);
    }
    if (input.aliases) {
      request.aliases = std::move(*input.aliases);
    }
    if (input.parentId) {
      request.parent_id = id_text(*input.parentId);
    }
    request.clear_parent = input.clearParent.value_or(false);

    const auto result = repository_->update_tag(tag_id, request);
    if (!result) {
      fail_repository(result.error());
    }

    return gql::service::AwaitableObject<
        std::shared_ptr<gql::tagimg::object::Tag>>(
        std::make_shared<gql::tagimg::object::Tag>(
            std::make_shared<TagModel>(*result)));
  }

  gql::service::AwaitableScalar<int>
  applyUpdateImages(gql::tagimg::UpdateImagesInput input) const {
    ImageUpdateInput request;
    request.image_ids = id_list(input.imageIds);
    if (request.image_ids.empty()) {
      fail("INVALID_ARGUMENT", "imageIds must not be empty");
    }
    if (const auto error =
            validate_uuid_list(request.image_ids, "imageIds")) {
      fail("INVALID_ARGUMENT", *error);
    }
    const std::set<std::string> unique_images(request.image_ids.begin(),
                                              request.image_ids.end());
    if (unique_images.size() != request.image_ids.size()) {
      fail("INVALID_ARGUMENT", "imageIds contains duplicate items");
    }

    if (input.addTagIds) {
      request.add_tag_ids = id_list(*input.addTagIds);
      if (const auto error =
              validate_tag_update_ids(request.add_tag_ids, "addTagIds")) {
        fail("INVALID_ARGUMENT", *error);
      }
    }
    if (input.removeTagIds) {
      request.remove_tag_ids = id_list(*input.removeTagIds);
      if (const auto error =
              validate_tag_update_ids(request.remove_tag_ids, "removeTagIds")) {
        fail("INVALID_ARGUMENT", *error);
      }
    }
    if (request.add_tag_ids.empty() && request.remove_tag_ids.empty()) {
      fail("INVALID_ARGUMENT", "at least one tag change is required");
    }

    const std::set<std::string> add_tags(request.add_tag_ids.begin(),
                                         request.add_tag_ids.end());
    for (const std::string &tag_id : request.remove_tag_ids) {
      if (add_tags.count(tag_id) > 0) {
        fail("INVALID_ARGUMENT", "addTagIds and removeTagIds must not overlap");
      }
    }

    const auto result = repository_->update_images(request);
    if (!result) {
      fail_repository(result.error());
    }
    return gql::service::AwaitableScalar<int>(static_cast<int>(*result));
  }

  gql::service::AwaitableScalar<IdType>
  applyRescanArtwork(IdType artwork_id) const {
    const std::string text = id_text(artwork_id);
    if (!is_uuid(text)) {
      fail("INVALID_ARGUMENT", "artworkId must be a uuid string");
    }

    const std::string task_id = drogon::utils::getUuid();
    log_line("[rescan] task " + task_id + " queued: artwork " + text);

    RescanArtworkRequest request;
    request.artwork_id = text;
    const bool accepted = runtime_->submit(
        std::move(request),
        [task_id](const Result<JobOutcome, JobError> &result) {
          log_add_outcome(task_id, result);
        });
    if (!accepted) {
      fail("SERVICE_UNAVAILABLE", "job worker is not running");
    }

    return gql::service::AwaitableScalar<IdType>(
        IdType(IdType::OpaqueString(task_id)));
  }

private:
  // 默认排除只作用于浏览/搜索；按 ID 精确获取的查询应跳过。
  // 只有默认标签被显式精确 include 时才不排除；子标签不覆盖父标签。
  void apply_default_exclusions(
      const std::vector<std::string> &include_tag_ids,
      std::vector<std::string> &exclude_tag_ids) const {
    for (const std::string &default_id : default_excluded_tag_ids_) {
      const bool explicitly_included =
          std::any_of(include_tag_ids.begin(), include_tag_ids.end(),
                      [&default_id](const std::string &id) {
                        return same_uuid(id, default_id);
                      });
      if (explicitly_included) {
        continue;
      }
      const bool already_excluded =
          std::any_of(exclude_tag_ids.begin(), exclude_tag_ids.end(),
                      [&default_id](const std::string &id) {
                        return same_uuid(id, default_id);
                      });
      if (!already_excluded) {
        exclude_tag_ids.push_back(default_id);
      }
    }
  }

  std::shared_ptr<ServiceRuntime> runtime_;
  std::shared_ptr<const TagimgRepository> repository_;
  std::shared_ptr<ClipEmbedder> clip_embedder_;
  std::vector<std::string> default_excluded_tag_ids_;
};

Value make_map(MapType fields) {
  Value value(Type::Map);
  for (auto &[name, item] : fields) {
    value.emplace_back(std::move(name), std::move(item));
  }
  return value;
}

Value make_list(ListType items) {
  Value value(Type::List);
  for (Value &item : items) {
    value.emplace_back(std::move(item));
  }
  return value;
}

// cppgraphqlgen 把 resolver 抛出的异常包成
// "Field error name: <field> unknown error: <what>" 且没有 extensions。
// 这里把包装去掉，并把 "CODE: detail" 形式的消息拆成 message +
// extensions.code。
Value attach_error_codes(Value response) {
  if (response.type() != Type::Map) {
    return response;
  }

  static constexpr std::string_view kWrapper = "unknown error: ";
  static constexpr std::string_view kCodes[] = {
      "INVALID_ARGUMENT",    "SERVICE_UNAVAILABLE",  "AI_UNAVAILABLE",
      "INTERNAL_ERROR",      "QUERY_FAILED",         "UNKNOWN_TAG",
      "UNKNOWN_ARTIST",      "UNKNOWN_IMAGE",        "NOT_FOUND",
      "TIMEOUT",
      "CANCELLED",           "TAG_NAME_CONFLICT",    "TAG_PARENT_NOT_FOUND",
      "TAG_CYCLE"};

  auto fields = response.release<MapType>();
  for (auto &[name, value] : fields) {
    if (name != "errors" || value.type() != Type::List) {
      continue;
    }

    auto errors = value.release<ListType>();
    for (Value &error : errors) {
      if (error.type() != Type::Map) {
        continue;
      }

      auto parts = error.release<MapType>();
      for (auto &[key, part] : parts) {
        if (key != "message" || part.type() != Type::String) {
          continue;
        }

        std::string message = part.release<std::string>();
        if (const auto position = message.find(kWrapper);
            position != std::string::npos) {
          message = message.substr(position + kWrapper.size());
        }

        std::string code;
        for (const std::string_view candidate : kCodes) {
          const std::string prefix = std::string(candidate) + ": ";
          if (message.rfind(prefix, 0) == 0) {
            code = candidate;
            message = message.substr(prefix.size());
            break;
          }
        }

        part = Value(std::string(message));
        if (!code.empty()) {
          Value extensions(Type::Map);
          extensions.emplace_back("code", Value(std::string(code)));
          parts.emplace_back("extensions", std::move(extensions));
        }
      }
      error = make_map(std::move(parts));
    }
    value = make_list(std::move(errors));
  }
  return make_map(std::move(fields));
}

std::string error_body(const std::string &message) {
  nlohmann::json body;
  body["errors"] = nlohmann::json::array();
  body["errors"].push_back({{"message", message}});
  return body.dump();
}

std::string error_body_with_code(std::string code, std::string message,
                                 bool retryable = false) {
  nlohmann::json extensions;
  extensions["code"] = std::move(code);
  if (retryable) {
    extensions["retryable"] = true;
  }
  nlohmann::json body;
  body["errors"] = nlohmann::json::array();
  body["errors"].push_back(
      {{"message", std::move(message)}, {"extensions", std::move(extensions)}});
  return body.dump();
}

drogon::HttpResponsePtr json_response(std::string body,
                                      drogon::HttpStatusCode status) {
  auto response = drogon::HttpResponse::newHttpResponse();
  response->setStatusCode(status);
  response->setContentTypeString("application/json");
  response->setBody(std::move(body));
  return response;
}

} // namespace

struct GraphQLService::Impl
    : std::enable_shared_from_this<GraphQLService::Impl> {
  // GraphQL 查询固定 4 个 worker，配合 8 条数据库连接，剩余连接留给图片流
  // 与 JobWorker。排队上限 64，避免慢查询把内存和连接池拖垮。
  static constexpr std::size_t kWorkerCount = 4;
  static constexpr std::size_t kQueueLimit = 64;
  static constexpr auto kQueryTimeout = std::chrono::seconds(90);

  struct Job {
    std::uint64_t id = 0;
    std::shared_ptr<RequestContext> context;
    std::string query;
    std::string operation_name;
    std::string variables;
    std::function<void(const drogon::HttpResponsePtr &)> callback;
  };

  Impl(std::shared_ptr<ServiceRuntime> runtime_ref,
       std::shared_ptr<const TagimgRepository> repository_ref,
       std::shared_ptr<ClipEmbedder> embedder_ref,
       std::shared_ptr<ConnectionRegistry> registry_ref,
       std::vector<std::string> default_excluded_tag_ids)
      : runtime(std::move(runtime_ref)), repository(std::move(repository_ref)),
        clip_embedder(std::move(embedder_ref)),
        registry(std::move(registry_ref)) {
    if (!runtime || !repository || !clip_embedder) {
      throw std::invalid_argument(
          "GraphQLService requires a ServiceRuntime, a repository and a "
          "ClipEmbedder");
    }
    // 启动时解析出的 ID 已经唯一，这里再做一次大小写归一化并去重，
    // 保证与前端的 include 列表比较时不受大小写影响。
    std::vector<std::string> normalized;
    normalized.reserve(default_excluded_tag_ids.size());
    for (std::string &id : default_excluded_tag_ids) {
      std::transform(id.begin(), id.end(), id.begin(), [](unsigned char byte) {
        return static_cast<char>(std::tolower(byte));
      });
      if (std::find(normalized.begin(), normalized.end(), id) ==
          normalized.end()) {
        normalized.push_back(std::move(id));
      }
    }
    root = std::make_shared<Root>(runtime, repository, clip_embedder,
                                  std::move(normalized));
    query = std::make_shared<gql::tagimg::object::Query>(root);
    mutation = std::make_shared<gql::tagimg::object::Mutation>(root);
    operations = std::make_shared<gql::tagimg::Operations>(query, mutation);

    workers.reserve(kWorkerCount);
    for (std::size_t i = 0; i < kWorkerCount; ++i) {
      workers.emplace_back([this] { worker_loop(); });
    }
  }

  ~Impl() {
    shutdown();
  }

  std::string execute(const std::string &query_text,
                      std::string_view operation_name,
                      const std::string &variables_text,
                      const std::shared_ptr<RequestContext> &context) const {
    gql::peg::ast ast;
    try {
      ast = gql::peg::parseString(query_text);
    } catch (const std::exception &exception) {
      return error_body(std::string("invalid query: ") + exception.what());
    }
    if (!ast.root) {
      return error_body("failed to parse query");
    }

    Value variables;
    try {
      variables = gql::response::parseJSON(variables_text);
    } catch (const std::exception &exception) {
      return error_body(std::string("invalid variables: ") + exception.what());
    }

    auto state = std::make_shared<GraphQLRequestState>(context);
    Value result;
    try {
      result =
          operations
              ->resolve({ast, operation_name, std::move(variables), {}, state})
              .get();
    } catch (const std::exception &exception) {
      return error_body(exception.what());
    }
    return gql::response::toJSON(attach_error_codes(std::move(result)));
  }

  // 入队并注册取消回调。返回 false 时 job 未被移动，调用方仍可拿回 callback。
  bool enqueue(Job &&job) {
    auto context = job.context;
    std::uint64_t id = 0;
    {
      std::lock_guard lock(queue_mutex);
      if (stopping || queue.size() >= kQueueLimit) {
        return false;
      }
      id = next_job_id++;
      job.id = id;
      queue.push_back(std::move(job));
    }

    std::weak_ptr<Impl> weak = weak_from_this();
    context->add_cancel_handler([weak, id](CancelReason) {
      if (auto self = weak.lock()) {
        self->remove_queued(id);
      }
    });
    queue_cv.notify_one();
    return true;
  }

  void remove_queued(std::uint64_t id) {
    std::shared_ptr<RequestContext> removed;
    {
      std::lock_guard lock(queue_mutex);
      for (auto it = queue.begin(); it != queue.end(); ++it) {
        if (it->id == id) {
          removed = it->context;
          queue.erase(it);
          break;
        }
      }
    }
    if (removed && registry) {
      registry->unregister_context(removed);
    }
  }

  void worker_loop() {
    for (;;) {
      Job job;
      {
        std::unique_lock lock(queue_mutex);
        queue_cv.wait(lock,
                      [this] { return stopping || !queue.empty(); });
        if (stopping && queue.empty()) {
          return;
        }
        job = std::move(queue.front());
        queue.pop_front();
      }
      run_job(std::move(job));
    }
  }

  void run_job(Job job) {
    auto context = job.context;
    const auto finish = [this, &context] {
      if (registry) {
        registry->unregister_context(context);
      }
    };

    // 断开回调可能漏掉（例如连接在入队与注册之间失效），出队时再确认一次。
    (void)context->cancelled();
    switch (context->reason()) {
    case CancelReason::ClientClosed:
    case CancelReason::Shutdown:
      finish();
      return;
    case CancelReason::Timeout:
      finish();
      job.callback(json_response(
          error_body_with_code("TIMEOUT", "request timed out"),
          drogon::k200OK));
      return;
    case CancelReason::Explicit:
      finish();
      job.callback(json_response(
          error_body_with_code("CANCELLED", "request cancelled"),
          drogon::k200OK));
      return;
    case CancelReason::None:
      break;
    }

    std::string body;
    {
      DeadlineGuard guard(kQueryTimeout, [context] {
        context->cancel(CancelReason::Timeout);
      });
      body = execute(job.query, job.operation_name, job.variables, context);
    }

    finish();
    (void)context->cancelled();
    switch (context->reason()) {
    case CancelReason::ClientClosed:
    case CancelReason::Shutdown:
      return;
    case CancelReason::Timeout:
      job.callback(json_response(
          error_body_with_code("TIMEOUT", "request timed out"),
          drogon::k200OK));
      return;
    case CancelReason::Explicit:
      job.callback(json_response(
          error_body_with_code("CANCELLED", "request cancelled"),
          drogon::k200OK));
      return;
    case CancelReason::None:
      job.callback(json_response(std::move(body), drogon::k200OK));
      return;
    }
  }

  void shutdown() {
    std::list<Job> pending;
    {
      std::lock_guard lock(queue_mutex);
      if (!stopping) {
        stopping = true;
        pending.swap(queue);
      }
    }
    queue_cv.notify_all();

    for (auto &job : pending) {
      if (job.context) {
        job.context->cancel(CancelReason::Shutdown);
        if (registry) {
          registry->unregister_context(job.context);
        }
      }
    }

    for (auto &worker : workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workers.clear();
  }

  std::shared_ptr<ServiceRuntime> runtime;
  std::shared_ptr<const TagimgRepository> repository;
  std::shared_ptr<ClipEmbedder> clip_embedder;
  std::shared_ptr<ConnectionRegistry> registry;
  std::shared_ptr<Root> root;
  std::shared_ptr<gql::tagimg::object::Query> query;
  std::shared_ptr<gql::tagimg::object::Mutation> mutation;
  std::shared_ptr<gql::tagimg::Operations> operations;

  std::mutex queue_mutex;
  std::condition_variable queue_cv;
  std::list<Job> queue;
  std::vector<std::thread> workers;
  std::uint64_t next_job_id = 0;
  bool stopping = false;
};

GraphQLService::GraphQLService(
    std::shared_ptr<ServiceRuntime> runtime,
    std::shared_ptr<const TagimgRepository> tagimg_repository,
    std::shared_ptr<ClipEmbedder> clip_embedder,
    std::shared_ptr<ConnectionRegistry> connection_registry,
    std::vector<std::string> default_excluded_tag_ids)
    : impl_(std::make_shared<Impl>(
          std::move(runtime), std::move(tagimg_repository),
          std::move(clip_embedder), std::move(connection_registry),
          std::move(default_excluded_tag_ids))) {
}

GraphQLService::~GraphQLService() = default;

void GraphQLService::shutdown() {
  if (impl_) {
    impl_->shutdown();
  }
}

void GraphQLService::handle(
    const drogon::HttpRequestPtr &request,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) const {
  std::string content_type = request->getHeader("content-type");
  std::transform(
      content_type.begin(), content_type.end(), content_type.begin(),
      [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
  if (content_type.rfind("application/json", 0) != 0) {
    callback(json_response(error_body("Content-Type must be application/json"),
                           drogon::k415UnsupportedMediaType));
    return;
  }

  nlohmann::json body;
  try {
    body = nlohmann::json::parse(request->body());
  } catch (const nlohmann::json::exception &) {
    callback(json_response(error_body("request body is not valid json"),
                           drogon::k400BadRequest));
    return;
  }
  if (!body.is_object()) {
    callback(json_response(error_body("request body must be an object"),
                           drogon::k400BadRequest));
    return;
  }

  const auto query = body.find("query");
  if (query == body.end() || !query->is_string() ||
      query->get_ref<const std::string &>().empty()) {
    callback(
        json_response(error_body("query is required"), drogon::k400BadRequest));
    return;
  }

  std::string operation_name;
  const auto operation = body.find("operationName");
  if (operation != body.end() && !operation->is_null()) {
    if (!operation->is_string()) {
      callback(json_response(error_body("operationName must be a string"),
                             drogon::k400BadRequest));
      return;
    }
    operation_name = operation->get<std::string>();
  }

  std::string variables_text = "{}";
  const auto variables = body.find("variables");
  if (variables != body.end() && !variables->is_null()) {
    if (!variables->is_object()) {
      callback(json_response(error_body("variables must be an object"),
                             drogon::k400BadRequest));
      return;
    }
    variables_text = variables->dump();
  }

  GraphQLService::Impl::Job job;
  job.context = std::make_shared<RequestContext>(request->getConnectionPtr());
  job.query = query->get<std::string>();
  job.operation_name = std::move(operation_name);
  job.variables = std::move(variables_text);
  job.callback = std::move(callback);

  const auto context = job.context;
  if (impl_->registry) {
    impl_->registry->register_context(context);
  }

  if (!impl_->enqueue(std::move(job))) {
    if (impl_->registry) {
      impl_->registry->unregister_context(context);
    }
    auto response = json_response(
        error_body_with_code("SERVICE_UNAVAILABLE", "server is busy", true),
        drogon::k503ServiceUnavailable);
    response->addHeader("Retry-After", "1");
    // enqueue 失败时 job 未被移动，callback 仍然有效。
    job.callback(std::move(response));
    return;
  }
}

} // namespace tagimg
