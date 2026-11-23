#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/result.hpp"
#include "image/image_metadata.hpp"

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
  UnknownImage,
  TagHierarchyMissing,
  TagNameConflict,
  TagParentNotFound,
  TagCycle,
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

// 一张图片扫描出的标签，用于按图片写入 image_tags。
struct ImageScannedTags {
  std::string image_id;
  std::vector<ArtworkTag> tags;
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

  // 模型扫描得到的逐图标签，与 entity_ids / image_ids 顺序一致。
  // 每项已经按置信度过滤并去重；没有扫描时为空数组。
  std::vector<std::vector<ArtworkTag>> scanned_tags;

  // Chinese-CLIP 图像向量，与 entity_ids 顺序一致。
  // 每项必须是 768 维、已 L2 归一化；空数组表示该图片不写向量。
  std::vector<std::vector<float>> embeddings;

  // 从图片读出的元数据，与 entity_ids 顺序一致。
  // 空数组表示不写元数据（例如调用方没有扫描结果）。
  std::vector<ImageMetadata> metadata;
};

// 用已有图片新建作品的输入。
//
// image_ids 必须已经存在于 images 表；只建立 artwork_images 关联，
// 不会插入新图片。rescan 为 true 时把扫描时间更新为当前时间。
struct ImageArtworkInput {
  std::string title;
  std::vector<std::string> image_ids;

  // 可选；std::nullopt 按 SQL NULL 写入。
  std::optional<std::string> description;
  std::optional<std::string> published_at;

  // 调用方直接指定、不经过扫描的标签与艺术家 ID。
  std::vector<std::string> tag_ids;
  std::vector<std::string> artist_ids;

  // 模型扫描得到的逐图标签，与 entity_ids / image_ids 顺序一致。
  // 每项已经按置信度过滤并去重；没有扫描时为空数组。
  std::vector<std::vector<ArtworkTag>> scanned_tags;

  // Chinese-CLIP 图像向量，与 image_ids 顺序一致。
  // rescan 时是本次重扫结果；否则保持空数组，不覆盖已有向量。
  std::vector<std::vector<float>> embeddings;

  // 从图片读出的元数据，与 image_ids 顺序一致。
  // rescan 时是本次重扫结果；否则保持空数组，不覆盖已有元数据。
  std::vector<ImageMetadata> metadata;

  // 是否更新图片的 scan_at。
  bool rescan = false;
};

struct SavedArtwork {
  std::string artwork_id;
  std::vector<std::string> image_ids;
};

// 一条待写入的图片向量。
struct ImageEmbeddingInput {
  std::string image_id;
  // 768 维、已 L2 归一化。
  std::vector<float> values;
};

// 一条待写入的图片元数据。
struct ImageMetadataInput {
  std::string image_id;
  ImageMetadata metadata;
};

// 回填时列出的一张缺向量的图片。
struct ImageEmbeddingRow {
  std::string image_id;
  // 为空表示无法重新下载原图。
  std::optional<std::string> entity_id;
};

// 查询结果里的标签信息。
struct ArtworkTagInfo {
  std::string id;
  // 标签自身的标准名。
  std::string name;
  // 直接父标签 ID；根的直接子标签为空。
  std::optional<std::string> parent_id;
  // 直接父标签的标准名；根标签为空。
  std::optional<std::string> category;
  // 从根标签到该标签自身、用 :: 连接的路径。
  std::string path;
  // 别名，按名称升序。
  std::vector<std::string> aliases;
  // 是否有直接子标签。
  bool has_children = false;
};

// 一条标签树搜索结果，带用于渲染树状结构的祖先链。
struct TagTreeMatchInfo {
  ArtworkTagInfo node;
  // 从根到父级，按根到父级排序。
  std::vector<ArtworkTagInfo> ancestors;
  // 命中的标准名或别名。
  std::string matched_name;
  bool matched_alias = false;
};

struct ArtworkArtistInfo {
  std::string id;
  std::string name;
};

// 一条标签搜索结果。
struct TagMatch {
  std::string id;
  // 命中的名称：标准名或别名。
  std::string name;
  // 从根标签到该标签自身、用 :: 连接的路径。
  std::string path;
};

// 创建标签的条件。
struct TagCreateInput {
  // 标准名，trim 后 1 到 255 个字符。
  std::string name;
  // 别名，按大小写不敏感去重。
  std::vector<std::string> aliases;
  // 直接父标签 ID；为空表示挂到根。
  std::optional<std::string> parent_id;
};

// 更新标签的条件；未提供的字段保持不变。
struct TagUpdateInput {
  // 标准名；为空表示不改。
  std::optional<std::string> name;
  // 别名；为空表示不改，空数组表示清空。
  std::optional<std::vector<std::string>> aliases;
  // 新父标签 ID；为空表示不改。
  std::optional<std::string> parent_id;
  // 是否把标签移到根；与 parent_id 互斥。
  bool clear_parent = false;
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
  // 与 tags 的顺序一致，按名称排序。
  std::vector<std::string> tag_ids;
  std::vector<ArtworkTagInfo> tags;
  std::vector<std::string> artist_ids;
  std::vector<ArtworkArtistInfo> artists;
  // 仅在按相似度排序时有值：作品内所有已向量化图片与该文本的最高余弦
  // 相似度，也就是排序依据。其它排序方式、或作品还没有向量时为空。
  std::optional<double> sort_score;
};

enum class ArtworkSortField {
  CreatedAt,
  PublishedAt,
  Title,
  // 按 Chinese-CLIP 文本向量与作品图片向量的余弦相似度倒序。
  Similarity,
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
  // 仅 order_by == Similarity 时使用：768 维文本向量，已 L2 归一化。
  // 没有对应图片向量的作品仍会返回，只是排在最后。
  std::vector<float> sort_embedding;
};

struct ArtworkSearchResult {
  std::vector<ArtworkSummary> items;
  bool has_more = false;
  // 仅 include_total 为 true 时有值。
  std::optional<std::size_t> total;
};

// 图片结果里附带的所属作品摘要。
struct ImageArtworkInfo {
  std::string id;
  std::string title;
  std::optional<std::string> published_at;
};

// 一次图片查询返回的单张图片。
struct ImageSummary {
  std::string id;
  std::optional<int> width;
  std::optional<int> height;
  std::optional<int> frame_count;
  // RFC3339（UTC，精确到秒）。
  std::string created_at;
  std::optional<std::string> scan_at;
  // 所属作品发布时间中的最大值；没有作品或都没填时为空。
  std::optional<std::string> published_at;
  // 与 tags 的顺序一致，按名称排序。
  std::vector<std::string> tag_ids;
  std::vector<ArtworkTagInfo> tags;
  // 所属作品，按 publishedAt DESC NULLS LAST, createdAt DESC, id 排序。
  std::vector<ImageArtworkInfo> artworks;
  // 仅在按相似度排序时有值：该图片与文本向量的余弦相似度。
  std::optional<double> sort_score;
};

enum class ImageSortField {
  CreatedAt,
  PublishedAt,
  ScanAt,
  // 按 Chinese-CLIP 文本向量与图片向量的余弦相似度倒序。
  Similarity,
};

// 一次图片查询的条件。与作品查询基本相同，但没有标题。
struct ImageSearchQuery {
  // 空数组表示不过滤。
  std::vector<std::string> entity_ids;
  std::vector<std::string> image_ids;
  std::vector<std::string> artist_ids;
  std::vector<std::string> include_tag_ids;
  bool include_all_tags = true;
  bool expand_include_tag_descendants = false;
  std::vector<std::string> exclude_tag_ids;
  bool expand_exclude_tag_descendants = false;
  ImageSortField order_by = ImageSortField::CreatedAt;
  bool descending = true;
  std::size_t limit = 20;
  std::size_t offset = 0;
  bool include_total = false;
  // 仅 order_by == Similarity 时使用：768 维文本向量，已 L2 归一化。
  // 文字相似度模式下没有向量的图片仍会返回，只是排在最后。
  std::vector<float> sort_embedding;
  // 仅参考图相似度模式：排除该图片自身，并要求结果图片都有向量。
  // 设置时 order_by 必须是 Similarity，且 sort_embedding 必须是对应参考图的向量。
  std::optional<std::string> reference_image_id;
};

struct ImageSearchResult {
  std::vector<ImageSummary> items;
  bool has_more = false;
  // 仅 include_total 为 true 时有值。
  std::optional<std::size_t> total;
};

// 一次批量图片更新的输入。
//
// 当前只支持标签增量：add/remove 都为空表示不修改。图片 ID 必须非空且不重复，
// 添加/移除的标签 ID 也必须不重复且互不重叠；任何图片或标签不存在都会
// 让整次更新失败。
struct ImageUpdateInput {
  std::vector<std::string> image_ids;
  std::vector<std::string> add_tag_ids;
  std::vector<std::string> remove_tag_ids;
};

// 一张不属于任何作品的图片。
struct OrphanImage {
  std::string id;
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

// 作品写入事务。
//
// begin_artwork_update() 打开事务并锁定作品行，之后所有写入都在同一个
// 连接、同一个事务里执行；直到 commit() 才提交，析构时未提交则自动回滚。
// 事务对象持有连接池租约，生命周期必须短：不要跨扫描、下载等耗时操作，
// 否则会占住池里的连接。
class ArtworkUpdateTransaction {
public:
  ~ArtworkUpdateTransaction();

  ArtworkUpdateTransaction(ArtworkUpdateTransaction &&) noexcept;
  ArtworkUpdateTransaction &operator=(ArtworkUpdateTransaction &&) = delete;
  ArtworkUpdateTransaction(const ArtworkUpdateTransaction &) = delete;
  ArtworkUpdateTransaction &operator=(const ArtworkUpdateTransaction &) =
      delete;

  // 设置标题，1 到 500 个字符。
  [[nodiscard]] Result<void, TagimgRepositoryError>
  set_title(std::string title);

  // 设置发布时间（RFC3339）；与 clear_published_at() 互斥。
  [[nodiscard]] Result<void, TagimgRepositoryError>
  set_published_at(std::string published_at);

  // 清空发布时间；与 set_published_at() 互斥。
  [[nodiscard]] Result<void, TagimgRepositoryError> clear_published_at();

  // 增量挂上/摘掉标签；标签必须已存在，否则返回 UnknownTag。
  [[nodiscard]] Result<void, TagimgRepositoryError>
  add_tag_ids(const std::vector<std::string> &tag_ids);
  [[nodiscard]] Result<void, TagimgRepositoryError>
  remove_tag_ids(const std::vector<std::string> &tag_ids);

  // 作品图片的完整顺序；集合必须与作品当前图片完全一致。
  [[nodiscard]] Result<void, TagimgRepositoryError>
  reorder_images(const std::vector<std::string> &image_ids);

  // 把逐图扫描标签按 (category, name) 解析成标签并写入 image_tags，
  // 只增不删。每个条目的 image_id 必须属于当前作品。
  // 缺失的标签会新建；分类标签必须已存在，否则返回 TagHierarchyMissing。
  // 返回这次涉及的标签数量（已按 tag_id 去重）。
  [[nodiscard]] Result<std::size_t, TagimgRepositoryError>
  add_scanned_tags(const std::vector<ImageScannedTags> &tags);

  // 按 image_id 替换向量；维度不是 768 时返回 InvalidArgument。
  [[nodiscard]] Result<std::size_t, TagimgRepositoryError>
  replace_embeddings(const std::vector<ImageEmbeddingInput> &items);

  // 把图片的 scan_at 更新为当前时间。
  [[nodiscard]] Result<std::size_t, TagimgRepositoryError>
  touch_image_scans(const std::vector<std::string> &image_ids);

  // 更新图片元数据并把 scan_at 更新为当前时间。
  // 每个条目的 image_id 必须属于当前作品。
  [[nodiscard]] Result<std::size_t, TagimgRepositoryError>
  update_image_metadata(const std::vector<ImageMetadataInput> &items);

  // 提交事务。没有做过任何修改时返回 InvalidArgument。
  [[nodiscard]] Result<void, TagimgRepositoryError> commit();

private:
  struct Impl;

  explicit ArtworkUpdateTransaction(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;

  friend class TagimgRepository;
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

  // 用已有图片新建作品。图片不存在或 ID 重复时返回 InvalidArgument。
  [[nodiscard]] Result<SavedArtwork, TagimgRepositoryError>
  save_artwork_from_images(const ImageArtworkInput &input) const;

  // 查询图片对应的实体 ID；图片不存在或 entity_id 为空时返回 std::nullopt。
  [[nodiscard]] Result<std::optional<std::string>, TagimgRepositoryError>
  find_image_entity_id(const std::string &image_id) const;

  // 判断实体是否已经作为图片入库。
  [[nodiscard]] Result<bool, TagimgRepositoryError>
  image_exists_by_entity_id(const std::string &entity_id) const;

  // 按作品 ID 查询；不存在时返回 std::nullopt。
  [[nodiscard]] Result<std::optional<ArtworkSummary>, TagimgRepositoryError>
  find_artwork(const std::string &artwork_id) const;

  // 分页查询作品。条件之间是 AND，同一数组内部是 OR。
  [[nodiscard]] Result<ArtworkSearchResult, TagimgRepositoryError>
  search_artworks(const ArtworkSearchQuery &query) const;

  // 分页查询图片。条件之间是 AND，同一数组内部是 OR。
  [[nodiscard]] Result<ImageSearchResult, TagimgRepositoryError>
  search_images(const ImageSearchQuery &query) const;

  // 批量更新图片；当前支持添加/移除标签，单事务原子执行。
  // 图片不存在返回 UnknownImage，标签不存在返回 UnknownTag。
  [[nodiscard]] Result<std::size_t, TagimgRepositoryError>
  update_images(const ImageUpdateInput &input) const;

  // 按标准名或别名搜索标签，用于输入联想。
  [[nodiscard]] Result<std::vector<TagMatch>, TagimgRepositoryError>
  search_tags(const TagSearchQuery &query) const;

  // 按父标签列出直接子标签；parent_id 为空表示列出所有顶层标签。
  [[nodiscard]] Result<std::vector<ArtworkTagInfo>, TagimgRepositoryError>
  list_tag_children(const std::optional<std::string> &parent_id) const;

  // 搜索标签树；命中节点的 ancestors 是从根到父级的完整链。
  [[nodiscard]] Result<std::vector<TagTreeMatchInfo>, TagimgRepositoryError>
  search_tag_tree(const std::string &text) const;

  // 创建标签。同父下标准名冲突时返回 TagNameConflict，父标签不存在时
  // 返回 TagParentNotFound。
  [[nodiscard]] Result<ArtworkTagInfo, TagimgRepositoryError>
  create_tag(const TagCreateInput &input) const;

  // 更新标签的标准名、别名和父级。标签不存在时返回 UnknownTag；父标签
  // 不存在时返回 TagParentNotFound；移动到自身或后代时返回 TagCycle。
  [[nodiscard]] Result<ArtworkTagInfo, TagimgRepositoryError>
  update_tag(const std::string &tag_id, const TagUpdateInput &input) const;

  // 批量删除作品；关联表级联清理，图片行保留。
  // 所有删除在同一个事务里完成；不存在的作品 ID 忽略，
  // 至少删除一个作品时 deleted=true。空数组返回 InvalidArgument。
  [[nodiscard]] Result<DeleteArtworkResult, TagimgRepositoryError>
  delete_artworks(const std::vector<std::string> &artwork_ids) const;

  // 打开作品写入事务；作品不存在时返回 std::nullopt。
  //
  // 事务打开后立即锁定该作品行，同一作品的并发写入因此串行执行。
  [[nodiscard]] Result<std::optional<ArtworkUpdateTransaction>,
                       TagimgRepositoryError>
  begin_artwork_update(const std::string &artwork_id) const;

  // 分页列出不属于任何作品的图片，按入库时间倒序。
  [[nodiscard]] Result<OrphanImageResult, TagimgRepositoryError>
  list_orphan_images(const OrphanImageQuery &query) const;

  // 批量写入图片向量（单事务，按 image_id upsert）。
  // 任一向量维度不是 768 时返回 InvalidArgument，不写任何数据。
  [[nodiscard]] Result<std::size_t, TagimgRepositoryError>
  save_clip_embeddings(const std::vector<ImageEmbeddingInput> &items) const;

  // 读取单张图片的向量；图片没有向量或不存在时返回 std::nullopt。
  [[nodiscard]] Result<std::optional<std::vector<float>>, TagimgRepositoryError>
  find_clip_embedding(const std::string &image_id) const;

  // 分页列出还没有向量的图片，按入库时间升序。
  [[nodiscard]] Result<std::vector<ImageEmbeddingRow>, TagimgRepositoryError>
  list_images_missing_embedding(std::size_t limit,
                                std::size_t offset) const;

private:
  struct Impl;
  explicit TagimgRepository(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

} // namespace tagimg
