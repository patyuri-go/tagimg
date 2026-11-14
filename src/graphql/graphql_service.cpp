#include "graphql/graphql_service.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <drogon/utils/Utilities.h>
#include <nlohmann/json.hpp>

#include "common/uuid.hpp"
#include "common/validation.hpp"
#include "graphql/ArtistObject.h"
#include "graphql/ArtworkConnectionObject.h"
#include "graphql/ArtworkObject.h"
#include "graphql/MutationObject.h"
#include "graphql/QueryObject.h"
#include "graphql/TagMatchObject.h"
#include "graphql/TagObject.h"
#include "graphql/TagPathObject.h"
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

// 简单的按行日志，串行输出避免多 worker 线程交错。
void log_line(std::string line) {
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  std::cout << line << std::endl;
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

  gql::service::AwaitableScalar<std::vector<IdType>> getImageIds() const {
    return gql::service::AwaitableScalar<std::vector<IdType>>(
        to_id_list(item_.image_ids));
  }

  gql::service::AwaitableScalar<std::vector<IdType>> getEntityIds() const {
    return gql::service::AwaitableScalar<std::vector<IdType>>(
        to_id_list(item_.entity_ids));
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

[[noreturn]] void fail_repository(TagimgRepositoryError error) {
  switch (error) {
  case TagimgRepositoryError::InvalidArgument:
    fail("INVALID_ARGUMENT", "invalid argument");
  case TagimgRepositoryError::UnknownTag:
    fail("UNKNOWN_TAG", "tag id does not exist");
  case TagimgRepositoryError::UnknownArtist:
    fail("UNKNOWN_ARTIST", "artist id does not exist");
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
  Root(std::shared_ptr<ScanService> scan_service,
       std::shared_ptr<const TagimgRepository> repository)
      : scan_service_(std::move(scan_service)),
        repository_(std::move(repository)) {
    if (!scan_service_ || !repository_) {
      throw std::invalid_argument("Root requires ScanService and repository");
    }
  }

  gql::service::AwaitableScalar<std::string> getPing() const {
    return gql::service::AwaitableScalar<std::string>(std::string("pong"));
  }

  gql::service::AwaitableScalar<IdType>
  applyAddArtwork(gql::tagimg::AddArtworkInput input) const {
    AddRequest request = to_add_request(std::move(input));
    const std::string task_id = drogon::utils::getUuid();
    const std::size_t entity_count = request.entity_ids.size();

    log_line("[scan] task " + task_id +
             " queued: " + std::to_string(entity_count) + " entities");

    const bool accepted = scan_service_->submit(
        std::move(request),
        [task_id](const Result<JobOutcome, JobError> &result) {
          if (!result) {
            log_line("[scan] task " + task_id +
                     " failed: " + std::string(to_string(result.error())));
            return;
          }
          const auto *add = std::get_if<AddResult>(&*result);
          if (add == nullptr) {
            log_line("[scan] task " + task_id +
                     " finished with an unexpected outcome");
            return;
          }
          if (!add->failures.empty()) {
            for (const ScanFailure &failure : add->failures) {
              log_line("[scan] task " + task_id + " failed for entity " +
                       failure.entity_id + ": " +
                       std::string(to_string(failure.error)));
            }
            return;
          }
          log_line("[scan] task " + task_id + " finished: artwork " +
                   add->artwork_id + ", " +
                   std::to_string(add->image_ids.size()) + " image(s), " +
                   std::to_string(add->tag_count) + " tag(s)");
        });

    if (!accepted) {
      fail("SERVICE_UNAVAILABLE", "job worker is not running");
    }
    return gql::service::AwaitableScalar<IdType>(
        IdType(IdType::OpaqueString(task_id)));
  }

  gql::service::AwaitableObject<std::shared_ptr<gql::tagimg::object::Artwork>>
  getArtwork(IdType id) const {
    const std::string text = id_text(id);
    if (!is_uuid(text)) {
      fail("INVALID_ARGUMENT", "id is not a valid uuid");
    }

    const auto result = repository_->find_artwork(text);
    if (!result) {
      fail_repository(result.error());
    }
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
  getArtworks(std::unique_ptr<gql::tagimg::ArtworkFilter> filter,
              std::optional<gql::tagimg::ArtworkOrderField> order_by,
              std::optional<gql::tagimg::SortDirection> direction,
              std::optional<int> limit, std::optional<int> offset,
              std::optional<bool> include_total) const {
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

    switch (order_by.value_or(gql::tagimg::ArtworkOrderField::CREATED_AT)) {
    case gql::tagimg::ArtworkOrderField::CREATED_AT:
      query.order_by = ArtworkSortField::CreatedAt;
      break;
    case gql::tagimg::ArtworkOrderField::PUBLISHED_AT:
      query.order_by = ArtworkSortField::PublishedAt;
      break;
    case gql::tagimg::ArtworkOrderField::TITLE:
      query.order_by = ArtworkSortField::Title;
      break;
    }
    query.descending = direction.value_or(gql::tagimg::SortDirection::DESC) ==
                       gql::tagimg::SortDirection::DESC;

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

    const auto result = repository_->search_artworks(query);
    if (!result) {
      fail_repository(result.error());
    }

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
      std::vector<std::shared_ptr<gql::tagimg::object::TagMatch>>>
  getTags(std::string query, std::unique_ptr<gql::tagimg::TagSearchScope> scope,
          std::optional<int> limit) const {
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

private:
  std::shared_ptr<ScanService> scan_service_;
  std::shared_ptr<const TagimgRepository> repository_;
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
      "INVALID_ARGUMENT", "SERVICE_UNAVAILABLE", "INTERNAL_ERROR",
      "QUERY_FAILED",     "UNKNOWN_TAG",         "UNKNOWN_ARTIST"};

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

drogon::HttpResponsePtr json_response(std::string body,
                                      drogon::HttpStatusCode status) {
  auto response = drogon::HttpResponse::newHttpResponse();
  response->setStatusCode(status);
  response->setContentTypeString("application/json");
  response->setBody(std::move(body));
  return response;
}

} // namespace

struct GraphQLService::Impl {
  Impl(std::shared_ptr<ScanService> scan_service_ref,
       std::shared_ptr<const TagimgRepository> repository_ref)
      : scan_service(std::move(scan_service_ref)),
        repository(std::move(repository_ref)) {
    if (!scan_service || !repository) {
      throw std::invalid_argument(
          "GraphQLService requires a ScanService and a repository");
    }
    root = std::make_shared<Root>(scan_service, repository);
    query = std::make_shared<gql::tagimg::object::Query>(root);
    mutation = std::make_shared<gql::tagimg::object::Mutation>(root);
    operations = std::make_shared<gql::tagimg::Operations>(query, mutation);
  }

  std::string execute(const std::string &query_text,
                      std::string_view operation_name,
                      const std::string &variables_text) const {
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

    Value result;
    try {
      result = operations->resolve({ast, operation_name, std::move(variables)})
                   .get();
    } catch (const std::exception &exception) {
      return error_body(exception.what());
    }
    return gql::response::toJSON(attach_error_codes(std::move(result)));
  }

  std::shared_ptr<ScanService> scan_service;
  std::shared_ptr<const TagimgRepository> repository;
  std::shared_ptr<Root> root;
  std::shared_ptr<gql::tagimg::object::Query> query;
  std::shared_ptr<gql::tagimg::object::Mutation> mutation;
  std::shared_ptr<gql::tagimg::Operations> operations;
};

GraphQLService::GraphQLService(
    std::shared_ptr<ScanService> scan_service,
    std::shared_ptr<const TagimgRepository> tagimg_repository)
    : impl_(std::make_shared<Impl>(std::move(scan_service),
                                   std::move(tagimg_repository))) {
}

GraphQLService::~GraphQLService() = default;

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

  callback(json_response(
      impl_->execute(query->get<std::string>(), operation_name, variables_text),
      drogon::k200OK));
}

} // namespace tagimg
