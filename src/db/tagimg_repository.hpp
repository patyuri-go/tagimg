#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/result.hpp"

namespace tagimg {

struct TagimgDbConfig {
  std::string host;
  std::uint16_t port = 5432;
  std::string user;
  std::string password;
  std::string dbname;
};

enum class TagimgRepositoryError {
  InvalidArgument,
  ConnectionFailed,
  DuplicateImage,
  UnknownTag,
  UnknownArtist,
  TagHierarchyMissing,
  QueryFailed,
};

[[nodiscard]] std::string_view to_string(TagimgRepositoryError error) noexcept;

// 一条要挂到作品上的扫描标签。
//
// category 是 wd14scan 根标签下的二级分类名（general / character / rating）。
struct ArtworkTag {
  std::string name;
  std::string category;
};

// 一次作品入库的完整输入。
struct ArtworkInput {
  // 必填。
  std::string title;
  std::vector<std::string> entity_ids;

  // 可选；std::nullopt 按 SQL NULL 写入。
  std::optional<std::string> description;
  std::optional<std::string> published_at;

  // 调用方直接指定、不经过扫描的标签与艺术家 ID。
  std::vector<std::string> tag_ids;
  std::vector<std::string> artist_ids;

  // 模型扫描得到、已按置信度过滤并去重的标签。
  std::vector<ArtworkTag> scanned_tags;
};

struct SavedArtwork {
  std::string artwork_id;
  std::vector<std::string> image_ids;
};

// 查询结果里的标签信息。
struct ArtworkTagInfo {
  std::string id;
  // 标签自身的标准名。
  std::string name;
  // 直接父标签的标准名；根标签为空。
  std::optional<std::string> category;
  // 从根标签到该标签自身、用 :: 连接的路径。
  std::string path;
  // 别名，按名称升序。
  std::vector<std::string> aliases;
};

struct ArtworkArtistInfo {
  std::string id;
  std::string name;
};

// 一条标签搜索结果。
struct TagMatch {
  std::string id;
  // 标签自身的标准名。
  std::string name;
  // 从根标签到该标签自身、用 :: 连接的路径。
  std::string path;
};

// 标签搜索的父标签范围。
enum class TagSearchScope {
  // 不限父标签。
  Any,
  // 只查全局根的直接子标签。
  Root,
  // 查指定父标签的整棵子树，不含父标签自身。
  Subtree,
};

// 一次标签搜索的条件。
struct TagSearchQuery {
  std::string text;
  TagSearchScope scope = TagSearchScope::Any;
  // 仅 scope 为 Subtree 时使用。
  std::string parent_id;
  // Subtree 下是否只查直接子标签，默认整棵子树。
  bool direct_only = false;
  std::size_t limit = 10;
};

// 一次作品查询返回的单条作品。
struct ArtworkSummary {
  std::string id;
  std::string title;
  std::optional<std::string> description;
  std::optional<std::string> published_at;
  // RFC3339（UTC，精确到秒）。
  std::string created_at;
  std::string updated_at;
  std::vector<std::string> image_ids;
  std::vector<std::string> entity_ids;
  // 与 tags 的顺序一致，按名称排序。
  std::vector<std::string> tag_ids;
  std::vector<ArtworkTagInfo> tags;
  std::vector<std::string> artist_ids;
  std::vector<ArtworkArtistInfo> artists;
};

enum class ArtworkSortField {
  CreatedAt,
  PublishedAt,
  Title,
};

// 一次作品查询的条件。
struct ArtworkSearchQuery {
  // 为空表示不过滤。
  std::optional<std::string> title;
  // 空数组表示不过滤。
  std::vector<std::string> entity_ids;
  std::vector<std::string> artist_ids;
  std::vector<std::string> include_tag_ids;
  bool include_all_tags = true;
  bool expand_include_tag_descendants = false;
  std::vector<std::string> exclude_tag_ids;
  bool expand_exclude_tag_descendants = false;
  ArtworkSortField order_by = ArtworkSortField::CreatedAt;
  bool descending = true;
  std::size_t limit = 20;
  std::size_t offset = 0;
  bool include_total = false;
};

struct ArtworkSearchResult {
  std::vector<ArtworkSummary> items;
  bool has_more = false;
  // 仅 include_total 为 true 时有值。
  std::optional<std::size_t> total;
};

// 一张不属于任何作品的图片。
struct OrphanImage {
  std::string id;
  std::optional<std::string> entity_id;
  std::optional<int> width;
  std::optional<int> height;
  std::optional<int> frame_count;
  // RFC3339（UTC，精确到秒）。
  std::string created_at;
  std::optional<std::string> scan_at;
};

struct OrphanImageQuery {
  std::size_t limit = 24;
  std::size_t offset = 0;
  bool include_total = false;
};

struct OrphanImageResult {
  std::vector<OrphanImage> items;
  bool has_more = false;
  // 仅 include_total 为 true 时有值。
  std::optional<std::size_t> total;
};

struct DeleteArtworkResult {
  bool deleted = false;
  // 本次删除后新变成游离的图片数量，不含删除前就已游离的图片。
  std::size_t newly_orphaned_image_count = 0;
};

// 图片库（tagimg）的写入口。
//
// save_artwork() 在单个事务里写 artworks、images、artwork_images、
// artwork_tags、artwork_artists：要么全部成功，要么全部回滚。
// 实例内部维护小连接池，可被多线程调用。
class TagimgRepository {
public:
  static Result<std::unique_ptr<TagimgRepository>, TagimgRepositoryError>
  connect(TagimgDbConfig config, std::size_t pool_size = 4);

  TagimgRepository(const TagimgRepository &) = delete;
  TagimgRepository &operator=(const TagimgRepository &) = delete;
  TagimgRepository(TagimgRepository &&) = delete;
  TagimgRepository &operator=(TagimgRepository &&) = delete;

  ~TagimgRepository();

  // 写入一个作品及其图片、标签、艺术家关联。
  //
  // 扫描标签按 name + category 在 wd14scan 层级下查找标准名称：命中则复用
  // 已有标签，未命中则在对应分类下新建。wd14scan 根标签或分类标签不存在
  // 时返回 TagHierarchyMissing，不写任何数据。
  // 任意 entity_id 已在 images 表存在时返回 DuplicateImage。
  [[nodiscard]] Result<SavedArtwork, TagimgRepositoryError>
  save_artwork(const ArtworkInput &input) const;

  // 按作品 ID 查询；不存在时返回 std::nullopt。
  [[nodiscard]] Result<std::optional<ArtworkSummary>, TagimgRepositoryError>
  find_artwork(const std::string &artwork_id) const;

  // 分页查询作品。条件之间是 AND，同一数组内部是 OR。
  [[nodiscard]] Result<ArtworkSearchResult, TagimgRepositoryError>
  search_artworks(const ArtworkSearchQuery &query) const;

  // 按标准名搜索标签，用于输入联想。
  [[nodiscard]] Result<std::vector<TagMatch>, TagimgRepositoryError>
  search_tags(const TagSearchQuery &query) const;

  // 删除一个作品；关联表级联清理，图片行保留。
  // 作品不存在时 deleted=false，不视为错误。
  [[nodiscard]] Result<DeleteArtworkResult, TagimgRepositoryError>
  delete_artwork(const std::string &artwork_id) const;

  // 分页列出不属于任何作品的图片，按入库时间倒序。
  [[nodiscard]] Result<OrphanImageResult, TagimgRepositoryError>
  list_orphan_images(const OrphanImageQuery &query) const;

private:
  struct Impl;
  explicit TagimgRepository(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

} // namespace tagimg
