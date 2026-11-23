#include "db/tagimg_repository.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <soci/postgresql/soci-postgresql.h>
#include <soci/soci.h>
#include <soci/transaction.h>

#include "common/uuid.hpp"
#include "common/validation.hpp"
#include "db/postgres_util.hpp"

namespace tagimg {
namespace {

constexpr const char *kFindRootTagSql = R"sql(
SELECT t.id::text
FROM tags t
JOIN tag_name n ON n.tag_id = t.id
WHERE n.type = 'standard'
  AND n.name = 'wd14scan'
  AND t.parent_id IS NULL
ORDER BY t.id
LIMIT 1
)sql";

// 只在指定父标签下查找，避免误用其他分支里的同名标签。
constexpr const char *kFindChildTagSql = R"sql(
SELECT t.id::text
FROM tags t
JOIN tag_name n ON n.tag_id = t.id
WHERE n.type = 'standard'
  AND n.name = :name
  AND t.parent_id = :parent_id
LIMIT 1
)sql";

constexpr const char *kCountImageSql = R"sql(
SELECT count(*)
FROM images
WHERE entity_id = :entity_id
)sql";

constexpr const char *kCountImageByIdSql = R"sql(
SELECT count(*)
FROM images
WHERE id = :image_id
)sql";

constexpr const char *kFindImageEntitySql = R"sql(
SELECT entity_id::text
FROM images
WHERE id = :image_id
)sql";

// 没有元数据时的兼容路径：只更新时间。
constexpr const char *kTouchImageScanSql = R"sql(
UPDATE images
SET scan_at = now()
WHERE id = :image_id
)sql";

// 批量版本：作品内全部图片重扫成功后统一更新一次。
constexpr const char *kTouchImageScansSql = R"sql(
UPDATE images
SET scan_at = now()
WHERE id = ANY(CAST(:image_ids AS uuid[]))
)sql";

constexpr const char *kCountTagSql = R"sql(
SELECT count(*)
FROM tags
WHERE id = :tag_id
)sql";

constexpr const char *kCountArtistSql = R"sql(
SELECT count(*)
FROM artists
WHERE id = :artist_id
)sql";

// 删除前先记下这些作品的图片并集，删除后再统计其中有哪些失去了全部引用。
constexpr const char *kArtworkImageIdsBatchSql = R"sql(
SELECT DISTINCT image_id::text
FROM artwork_images
WHERE artwork_id = ANY(CAST(:artwork_ids AS uuid[]))
)sql";

constexpr const char *kDeleteArtworksSql = R"sql(
DELETE FROM artworks
WHERE id = ANY(CAST(:artwork_ids AS uuid[]))
RETURNING id::text
)sql";

constexpr const char *kCountOrphanAmongSql = R"sql(
SELECT count(*)
FROM images i
WHERE i.id = ANY(CAST(:image_ids AS uuid[]))
  AND NOT EXISTS (
    SELECT 1
    FROM artwork_images ai
    WHERE ai.image_id = i.id
  )
)sql";

// 图片删除会级联改变作品内容，因此先刷新受影响作品的更新时间。
constexpr const char *kTouchArtworksForImagesSql = R"sql(
UPDATE artworks
SET updated_at = now()
WHERE id IN (
  SELECT artwork_id
  FROM artwork_images
  WHERE image_id = ANY(CAST(:image_ids AS uuid[]))
)
)sql";

constexpr const char *kDeleteImagesSql = R"sql(
DELETE FROM images
WHERE id = ANY(CAST(:image_ids AS uuid[]))
RETURNING id::text
)sql";

// 编辑作品：先锁行，确保同一作品的并发更新串行执行。
constexpr const char *kLockArtworkSql = R"sql(
SELECT id::text
FROM artworks
WHERE id = :artwork_id
FOR UPDATE
)sql";

// 校验图片顺序时锁住当前作品的全部图片行。
constexpr const char *kLockArtworkImagesSql = R"sql(
SELECT image_id::text
FROM artwork_images
WHERE artwork_id = :artwork_id
FOR UPDATE
)sql";

// 按传入数组的顺序生成 position 0..n-1；调用方已校验集合完全一致。
constexpr const char *kUpdateArtworkImagePositionsSql = R"sql(
WITH ordered AS (
  SELECT image_id, ordinality - 1 AS position
  FROM unnest(CAST(:image_ids AS uuid[]))
       WITH ORDINALITY AS t(image_id, ordinality)
)
UPDATE artwork_images ai
SET position = ordered.position
FROM ordered
WHERE ai.artwork_id = :artwork_id
  AND ai.image_id = ordered.image_id
)sql";

constexpr const char *kUpdateArtworkTitleSql = R"sql(
UPDATE artworks
SET title = :title
WHERE id = :artwork_id
)sql";

constexpr const char *kUpdateArtworkPublishedAtSql = R"sql(
UPDATE artworks
SET published_at = CAST(:published_at AS timestamptz)
WHERE id = :artwork_id
)sql";

constexpr const char *kClearArtworkPublishedAtSql = R"sql(
UPDATE artworks
SET published_at = NULL
WHERE id = :artwork_id
)sql";

constexpr const char *kTouchArtworkSql = R"sql(
UPDATE artworks
SET updated_at = now()
WHERE id = :artwork_id
)sql";

constexpr const char *kCountExistingTagsSql = R"sql(
SELECT count(*)
FROM tags
WHERE id = ANY(CAST(:tag_ids AS uuid[]))
)sql";

constexpr const char *kCountExistingImagesSql = R"sql(
SELECT count(*)
FROM images
WHERE id = ANY(CAST(:image_ids AS uuid[]))
)sql";

constexpr const char *kInsertImageTagsForImagesSql = R"sql(
INSERT INTO image_tags (image_id, tag_id)
SELECT i.id, t.id
FROM images i
CROSS JOIN unnest(CAST(:tag_ids AS uuid[])) AS t(id)
WHERE i.id = ANY(CAST(:image_ids AS uuid[]))
ON CONFLICT DO NOTHING
)sql";

constexpr const char *kDeleteImageTagsForImagesSql = R"sql(
DELETE FROM image_tags
WHERE image_id = ANY(CAST(:image_ids AS uuid[]))
  AND tag_id = ANY(CAST(:tag_ids AS uuid[]))
)sql";

constexpr const char *kInsertImageTagsForArtworkSql = R"sql(
INSERT INTO image_tags (image_id, tag_id)
SELECT ai.image_id, t.id
FROM artwork_images ai
CROSS JOIN unnest(CAST(:tag_ids AS uuid[])) AS t(id)
WHERE ai.artwork_id = :artwork_id
ON CONFLICT DO NOTHING
)sql";

constexpr const char *kDeleteImageTagsSql = R"sql(
DELETE FROM image_tags it
USING artwork_images ai
WHERE it.image_id = ai.image_id
  AND ai.artwork_id = :artwork_id
  AND it.tag_id = ANY(CAST(:tag_ids AS uuid[]))
)sql";

constexpr const char *kOrphanImagesSql = R"sql(
SELECT i.id::text,
       i.width,
       i.height,
       i.frame_count,
       to_char(i.created_at AT TIME ZONE 'UTC',
               'YYYY-MM-DD"T"HH24:MI:SS"Z"'),
       CASE WHEN i.scan_at IS NULL THEN NULL
            ELSE to_char(i.scan_at AT TIME ZONE 'UTC',
                         'YYYY-MM-DD"T"HH24:MI:SS"Z"') END
FROM images i
WHERE NOT EXISTS (
  SELECT 1
  FROM artwork_images ai
  WHERE ai.image_id = i.id
)
ORDER BY i.created_at DESC, i.id DESC
LIMIT :limit OFFSET :offset
)sql";

constexpr const char *kCountOrphanImagesSql = R"sql(
SELECT count(*)
FROM images i
WHERE NOT EXISTS (
  SELECT 1
  FROM artwork_images ai
  WHERE ai.image_id = i.id
)
)sql";

constexpr const char *kInsertArtworkSql = R"sql(
INSERT INTO artworks (title, description, published_at)
VALUES (:title, :description, CAST(:published_at AS timestamptz))
RETURNING id::text
)sql";

constexpr const char *kInsertImageSql = R"sql(
INSERT INTO images
  (entity_id, width, height, frame_count, mime_type, taken_at, scan_at)
VALUES
  (:entity_id, :width, :height, :frame_count, :mime_type,
   CAST(:taken_at AS timestamp), now())
RETURNING id::text
)sql";

constexpr const char *kUpdateImageMetadataSql = R"sql(
UPDATE images
SET width = :width,
    height = :height,
    frame_count = :frame_count,
    mime_type = :mime_type,
    taken_at = CAST(:taken_at AS timestamp),
    scan_at = now()
WHERE id = :image_id
)sql";

// 表名含含义但全为下划线，不需要双引号。
constexpr std::size_t kClipEmbeddingDim = 768;

constexpr const char *kUpsertClipEmbeddingSql = R"sql(
INSERT INTO chinese_clip_vit_large_patch14 (image_id, embedding)
VALUES (:image_id, CAST(:embedding AS vector))
ON CONFLICT (image_id) DO UPDATE
SET embedding = EXCLUDED.embedding, updated_at = now()
)sql";

constexpr const char *kFindClipEmbeddingSql = R"sql(
SELECT embedding::text
FROM chinese_clip_vit_large_patch14
WHERE image_id = :image_id
)sql";

constexpr const char *kListMissingClipEmbeddingSql = R"sql(
SELECT i.id::text, i.entity_id::text
FROM images i
LEFT JOIN chinese_clip_vit_large_patch14 c ON c.image_id = i.id
WHERE c.image_id IS NULL
ORDER BY i.created_at
LIMIT :limit OFFSET :offset
)sql";

constexpr const char *kInsertArtworkImageSql = R"sql(
INSERT INTO artwork_images (artwork_id, image_id, position)
VALUES (:artwork_id, :image_id, :position)
)sql";

constexpr const char *kInsertImageTagSql = R"sql(
INSERT INTO image_tags (image_id, tag_id)
VALUES (:image_id, :tag_id)
ON CONFLICT DO NOTHING
)sql";

constexpr const char *kInsertArtworkArtistSql = R"sql(
INSERT INTO artwork_artists (artwork_id, artist_id)
VALUES (:artwork_id, :artist_id)
ON CONFLICT DO NOTHING
)sql";

constexpr const char *kInsertTagSql = R"sql(
INSERT INTO tags (parent_id)
VALUES (:parent_id)
RETURNING id::text
)sql";

constexpr const char *kInsertTagNameSql = R"sql(
INSERT INTO tag_name (tag_id, type, name)
VALUES (:tag_id, 'standard'::tag_name_type, :name)
)sql";

constexpr const char *kInsertAliasNameSql = R"sql(
INSERT INTO tag_name (tag_id, type, name)
VALUES (:tag_id, 'alias'::tag_name_type, :name)
)sql";

constexpr const char *kDeleteTagAliasesSql = R"sql(
DELETE FROM tag_name
WHERE tag_id = :tag_id AND type = 'alias'
)sql";

// 取该标签的第一条标准名；脏数据里有多条时只认 id 最小的一条。
constexpr const char *kSelectStandardNameSql = R"sql(
SELECT id::text, name
FROM tag_name
WHERE tag_id = :tag_id AND type = 'standard'
ORDER BY id
LIMIT 1
)sql";

constexpr const char *kUpdateTagNameSql = R"sql(
UPDATE tag_name
SET name = :name
WHERE id = :id
)sql";

// 锁定标签行，同时取当前父级；空字符串表示根。
constexpr const char *kSelectTagForUpdateSql = R"sql(
SELECT COALESCE(parent_id::text, '')
FROM tags
WHERE id = :tag_id
FOR UPDATE
)sql";

constexpr const char *kUpdateTagParentSql = R"sql(
UPDATE tags
SET parent_id = :parent_id
WHERE id = :tag_id
)sql";

// 同一父级下是否已有同名标准名；parent_id 为空表示根。
constexpr const char *kCountCreateNameConflictSql = R"sql(
WITH params AS (
  SELECT CAST(:parent_id AS uuid) AS parent_id,
         CAST(:name AS text) AS name
)
SELECT count(*)
FROM params p
JOIN tags t
  ON (p.parent_id IS NULL AND t.parent_id IS NULL)
  OR t.parent_id = p.parent_id
JOIN tag_name n ON n.tag_id = t.id
WHERE n.type = 'standard' AND lower(n.name) = lower(p.name)
)sql";

// 改名/移动时检查最终父级下的同名标准名，排除标签自身。
constexpr const char *kCountUpdateNameConflictSql = R"sql(
WITH params AS (
  SELECT CAST(:parent_id AS uuid) AS parent_id,
         CAST(:tag_id AS uuid) AS tag_id,
         CAST(:name AS text) AS name
)
SELECT count(*)
FROM params p
JOIN tags t
  ON (p.parent_id IS NULL AND t.parent_id IS NULL)
  OR t.parent_id = p.parent_id
JOIN tag_name n ON n.tag_id = t.id
WHERE n.type = 'standard'
  AND lower(n.name) = lower(p.name)
  AND t.id <> p.tag_id
)sql";

// 当前标签是否已有与给定标准名相同的别名。
constexpr const char *kCountTagAliasNameSql = R"sql(
SELECT count(*)
FROM tag_name
WHERE tag_id = :tag_id
  AND type = 'alias'
  AND lower(name) = lower(:name)
)sql";

// 从新父级向上回溯，若遇到标签自身则说明成环。
constexpr const char *kCheckTagCycleSql = R"sql(
WITH RECURSIVE ancestors(id) AS (
  SELECT CAST(:parent_id AS uuid)
  UNION ALL
  SELECT t.parent_id
  FROM tags t
  JOIN ancestors a ON t.id = a.id
  WHERE t.parent_id IS NOT NULL
)
SELECT count(*)
FROM ancestors
WHERE id = CAST(:tag_id AS uuid)
)sql";

// 按父级串行化创建/移动。根节点用固定 key，其它父级用父级 ID。
constexpr const char *kAcquireTagLockSql = R"sql(
WITH lock AS (
  SELECT pg_advisory_xact_lock(hashtextextended(:key, 0))
)
SELECT 1 FROM lock
)sql";

std::optional<std::string> find_root_tag(soci::session &sql) {
  soci::rowset<soci::row> rows = (sql.prepare << kFindRootTagSql);
  const auto row = rows.begin();
  if (row == rows.end()) {
    return std::nullopt;
  }
  return row->get<std::string>(0);
}

std::optional<std::string> find_child_tag(soci::session &sql,
                                          const std::string &parent_id,
                                          const std::string &name) {
  soci::rowset<soci::row> rows =
      (sql.prepare << kFindChildTagSql, soci::use(name), soci::use(parent_id));
  const auto row = rows.begin();
  if (row == rows.end()) {
    return std::nullopt;
  }
  return row->get<std::string>(0);
}

long long count_rows(soci::session &sql, const char *statement,
                     const std::string &value) {
  long long count = 0;
  sql << statement, soci::use(value), soci::into(count);
  return count;
}

std::string insert_returning_id(soci::session &sql, const char *statement,
                                const std::string &value) {
  soci::rowset<soci::row> rows = (sql.prepare << statement, soci::use(value));
  const auto row = rows.begin();
  if (row == rows.end()) {
    return {};
  }
  return row->get<std::string>(0);
}

// 可空元数据字段的 SOCI 绑定值。
struct ImageMetadataBinding {
  int width = 0;
  soci::indicator width_indicator = soci::i_null;
  int height = 0;
  soci::indicator height_indicator = soci::i_null;
  int frame_count = 0;
  soci::indicator frame_count_indicator = soci::i_null;
  std::string mime_type;
  soci::indicator mime_type_indicator = soci::i_null;
  std::string taken_at;
  soci::indicator taken_at_indicator = soci::i_null;
};

ImageMetadataBinding make_image_metadata_binding(const ImageMetadata &metadata) {
  ImageMetadataBinding binding;
  if (metadata.width) {
    binding.width = *metadata.width;
    binding.width_indicator = soci::i_ok;
  }
  if (metadata.height) {
    binding.height = *metadata.height;
    binding.height_indicator = soci::i_ok;
  }
  if (metadata.frame_count) {
    binding.frame_count = *metadata.frame_count;
    binding.frame_count_indicator = soci::i_ok;
  }
  if (metadata.mime_type) {
    binding.mime_type = *metadata.mime_type;
    binding.mime_type_indicator = soci::i_ok;
  }
  if (metadata.taken_at) {
    binding.taken_at = *metadata.taken_at;
    binding.taken_at_indicator = soci::i_ok;
  }
  return binding;
}

bool is_valid_image_metadata(const ImageMetadata &metadata) {
  return (!metadata.width || *metadata.width > 0) &&
         (!metadata.height || *metadata.height > 0) &&
         (!metadata.frame_count || *metadata.frame_count > 0) &&
         (!metadata.mime_type || !metadata.mime_type->empty()) &&
         (!metadata.taken_at || !metadata.taken_at->empty());
}

std::string insert_image_returning_id(soci::session &sql,
                                      const std::string &entity_id,
                                      const ImageMetadata &metadata) {
  ImageMetadataBinding binding = make_image_metadata_binding(metadata);
  soci::rowset<soci::row> rows =
      (sql.prepare << kInsertImageSql, soci::use(entity_id),
       soci::use(binding.width, binding.width_indicator),
       soci::use(binding.height, binding.height_indicator),
       soci::use(binding.frame_count, binding.frame_count_indicator),
       soci::use(binding.mime_type, binding.mime_type_indicator),
       soci::use(binding.taken_at, binding.taken_at_indicator));
  const auto row = rows.begin();
  if (row == rows.end()) {
    return {};
  }
  return row->get<std::string>(0);
}

void update_image_metadata_row(soci::session &sql, const std::string &image_id,
                               const ImageMetadata &metadata) {
  ImageMetadataBinding binding = make_image_metadata_binding(metadata);
  sql << kUpdateImageMetadataSql,
      soci::use(binding.width, binding.width_indicator),
      soci::use(binding.height, binding.height_indicator),
      soci::use(binding.frame_count, binding.frame_count_indicator),
      soci::use(binding.mime_type, binding.mime_type_indicator),
      soci::use(binding.taken_at, binding.taken_at_indicator),
      soci::use(image_id);
}

// 把 UUID 列表拼成 PostgreSQL 数组字面量；调用前必须校验过 UUID 格式。
std::string uuid_array_literal(const std::vector<std::string> &ids) {
  std::string literal = "{";
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (i > 0) {
      literal.push_back(',');
    }
    literal += ids[i];
  }
  literal.push_back('}');
  return literal;
}

// 把向量拼成 pgvector 文本字面量 [a,b,c]。
// 保留 9 位有效数字，float32 精度足够，避免无意义的尾数。
std::string pgvector_literal(const std::vector<float> &values) {
  std::string literal = "[";
  literal.reserve(values.size() * 12 + 2);
  char buffer[32];
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      literal.push_back(',');
    }
    const int written = std::snprintf(buffer, sizeof(buffer), "%.9g",
                                      static_cast<double>(values[i]));
    if (written > 0) {
      literal.append(buffer, static_cast<std::size_t>(written));
    }
  }
  literal.push_back(']');
  return literal;
}

std::string escape_like(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (const char c : text) {
    if (c == '\\' || c == '%' || c == '_') {
      escaped.push_back('\\');
    }
    escaped.push_back(c);
  }
  return escaped;
}

std::string like_pattern(std::string_view text) {
  std::string pattern;
  pattern.reserve(text.size() + 2);
  pattern.push_back('%');
  pattern += escape_like(text);
  pattern.push_back('%');
  return pattern;
}

// 前缀匹配，用于给「完全相等 > 前缀 > 其他」排序。
std::string like_prefix_pattern(std::string_view text) {
  std::string pattern = escape_like(text);
  pattern.push_back('%');
  return pattern;
}

std::vector<std::string> unique_ids(const std::vector<std::string> &ids) {
  const std::set<std::string> ordered(ids.begin(), ids.end());
  return std::vector<std::string>(ordered.begin(), ordered.end());
}

std::string trim_ascii(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  const auto is_space = [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
  };
  while (begin < end && is_space(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin &&
         is_space(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

// 名称比较只需要覆盖 ASCII 大小写；非 ASCII 字节原样保留。
std::string ascii_lower(std::string_view text) {
  std::string lowered;
  lowered.reserve(text.size());
  for (const unsigned char c : text) {
    lowered.push_back(static_cast<char>(std::tolower(c)));
  }
  return lowered;
}

bool has_control_char(std::string_view text) {
  for (std::size_t i = 0; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x20 || c == 0x7F) {
      return true;
    }
    // UTF-8 编码的 C1 控制字符：C2 80..9F。
    if (c == 0xC2 && i + 1 < text.size()) {
      const unsigned char next = static_cast<unsigned char>(text[i + 1]);
      if (next >= 0x80 && next <= 0x9F) {
        return true;
      }
    }
  }
  return false;
}

bool valid_tag_name(std::string_view name) {
  if (name.empty() || utf8_length(name) > kMaxTagNameLength) {
    return false;
  }
  if (name.find("::") != std::string_view::npos) {
    return false;
  }
  return !has_control_char(name);
}

// trim、校验并按大小写不敏感去重；别名不能等于自身标准名。
Result<std::vector<std::string>, TagimgRepositoryError>
normalize_tag_aliases(const std::vector<std::string> &aliases,
                      const std::string &standard_name) {
  std::vector<std::string> normalized;
  std::set<std::string> seen;
  const std::string standard_key = ascii_lower(standard_name);
  for (const std::string &raw : aliases) {
    std::string alias = trim_ascii(raw);
    if (!valid_tag_name(alias)) {
      return TagimgRepositoryError::InvalidArgument;
    }
    const std::string key = ascii_lower(alias);
    if (!standard_name.empty() && key == standard_key) {
      return TagimgRepositoryError::TagNameConflict;
    }
    if (!seen.insert(key).second) {
      continue;
    }
    normalized.push_back(std::move(alias));
  }
  return normalized;
}

// 实体模式与图片模式共用的写库参数。
struct ArtworkWriteInput {
  std::string title;
  std::optional<std::string> description;
  std::optional<std::string> published_at;
  std::vector<std::string> tag_ids;
  std::vector<std::string> artist_ids;
  // 与 entity_ids（或 image_ids）顺序一致；每项是该图的扫描标签。
  std::vector<std::vector<ArtworkTag>> scanned_tags;
  // 与 entity_ids（或 image_ids）顺序一致；空向量表示该图不写向量。
  std::vector<std::vector<float>> embeddings;
  // 与 entity_ids（或 image_ids）顺序一致；空数组表示不写元数据。
  std::vector<ImageMetadata> metadata;
};

// 在单个事务里写作品、图片关联、标签与艺术家。
//
// new_entity_ids 非空时按实体新建图片；否则把 existing_image_ids
// 里已经存在的图片直接关联到新作品。两种模式不会同时使用。
Result<SavedArtwork, TagimgRepositoryError>
write_artwork(soci::session &sql, const ArtworkWriteInput &input,
              const std::vector<std::string> &new_entity_ids,
              const std::vector<std::string> &existing_image_ids,
              bool touch_scan_at) {
  if (new_entity_ids.empty() && existing_image_ids.empty()) {
    return TagimgRepositoryError::InvalidArgument;
  }

  const std::size_t image_count =
      new_entity_ids.empty() ? existing_image_ids.size() : new_entity_ids.size();
  if (!input.scanned_tags.empty() && input.scanned_tags.size() != image_count) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (!input.metadata.empty() && input.metadata.size() != image_count) {
    return TagimgRepositoryError::InvalidArgument;
  }
  for (const ImageMetadata &metadata : input.metadata) {
    if (!is_valid_image_metadata(metadata)) {
      return TagimgRepositoryError::InvalidArgument;
    }
  }

  try {
    soci::transaction transaction(sql);

    const auto root_id = find_root_tag(sql);
    if (!root_id) {
      return TagimgRepositoryError::TagHierarchyMissing;
    }

    // 先把用到的分类标签全部解析出来；缺任何一个都不写库。
    std::map<std::string, std::string> categories;
    for (const auto &image_tags : input.scanned_tags) {
      for (const ArtworkTag &tag : image_tags) {
        if (categories.find(tag.category) != categories.end()) {
          continue;
        }
        const auto category_id = find_child_tag(sql, *root_id, tag.category);
        if (!category_id) {
          return TagimgRepositoryError::TagHierarchyMissing;
        }
        categories.emplace(tag.category, *category_id);
      }
    }

    std::string title = input.title;
    std::string description = input.description.value_or(std::string{});
    soci::indicator description_indicator =
        input.description.has_value() ? soci::i_ok : soci::i_null;
    std::string published_at = input.published_at.value_or(std::string{});
    soci::indicator published_indicator =
        input.published_at.has_value() ? soci::i_ok : soci::i_null;

    SavedArtwork saved;
    {
      soci::rowset<soci::row> rows =
          (sql.prepare << kInsertArtworkSql, soci::use(title),
           soci::use(description, description_indicator),
           soci::use(published_at, published_indicator));
      const auto row = rows.begin();
      if (row == rows.end()) {
        return TagimgRepositoryError::QueryFailed;
      }
      saved.artwork_id = row->get<std::string>(0);
    }

    saved.image_ids.reserve(new_entity_ids.size() + existing_image_ids.size());
    if (!new_entity_ids.empty()) {
      for (std::size_t index = 0; index < new_entity_ids.size(); ++index) {
        const std::string &entity_id = new_entity_ids[index];
        if (count_rows(sql, kCountImageSql, entity_id) > 0) {
          return TagimgRepositoryError::DuplicateImage;
        }

        std::string image_id =
            insert_image_returning_id(
                sql, entity_id,
                input.metadata.empty() ? ImageMetadata{} : input.metadata[index]);
        if (image_id.empty()) {
          return TagimgRepositoryError::QueryFailed;
        }

        const int position = static_cast<int>(index);
        sql << kInsertArtworkImageSql, soci::use(saved.artwork_id),
            soci::use(image_id), soci::use(position);

        // 向量与图片行同事务写入；维度不符直接回滚整单。
        if (index < input.embeddings.size() &&
            !input.embeddings[index].empty()) {
          const auto &values = input.embeddings[index];
          if (values.size() != 768) {
            return TagimgRepositoryError::InvalidArgument;
          }
          const std::string literal = pgvector_literal(values);
          sql << kUpsertClipEmbeddingSql, soci::use(image_id),
              soci::use(literal);
        }

        saved.image_ids.push_back(std::move(image_id));
      }
    } else {
      for (std::size_t index = 0; index < existing_image_ids.size(); ++index) {
        const std::string &image_id = existing_image_ids[index];
        if (count_rows(sql, kCountImageByIdSql, image_id) == 0) {
          return TagimgRepositoryError::InvalidArgument;
        }

        const int position = static_cast<int>(index);
        sql << kInsertArtworkImageSql, soci::use(saved.artwork_id),
            soci::use(image_id), soci::use(position);
        if (touch_scan_at) {
          if (input.metadata.empty()) {
            sql << kTouchImageScanSql, soci::use(image_id);
          } else {
            update_image_metadata_row(sql, image_id, input.metadata[index]);
          }
        }
        saved.image_ids.push_back(image_id);
      }
    }

    // 调用方直接指定的标签：必须已经存在，并一次挂到作品的所有图片上。
    if (!input.tag_ids.empty()) {
      std::vector<std::string> tag_ids;
      tag_ids.reserve(input.tag_ids.size());
      for (const std::string &id : input.tag_ids) {
        if (count_rows(sql, kCountTagSql, id) == 0) {
          return TagimgRepositoryError::UnknownTag;
        }
        tag_ids.push_back(id);
      }
      sql << kInsertImageTagsForArtworkSql, soci::use(tag_ids),
          soci::use(saved.artwork_id);
    }

    // 调用方直接指定的艺术家：必须已经存在。
    for (const std::string &id : input.artist_ids) {
      std::string artist_id = id;
      if (count_rows(sql, kCountArtistSql, artist_id) == 0) {
        return TagimgRepositoryError::UnknownArtist;
      }
      sql << kInsertArtworkArtistSql, soci::use(saved.artwork_id),
          soci::use(artist_id);
    }

    // 扫描标签：按「父标签 + 标准名称」查找，找不到才在分类下新建。
    // 每张图的标签只写到该图的 image_tags，不再合并到作品级。
    if (!input.scanned_tags.empty()) {
      for (std::size_t index = 0; index < saved.image_ids.size(); ++index) {
        const std::string &image_id = saved.image_ids[index];
        for (const ArtworkTag &tag : input.scanned_tags[index]) {
          const std::string &category_id = categories.at(tag.category);
          std::string name = tag.name;

          auto tag_id = find_child_tag(sql, category_id, name);
          if (!tag_id) {
            std::string created =
                insert_returning_id(sql, kInsertTagSql, category_id);
            if (created.empty()) {
              return TagimgRepositoryError::QueryFailed;
            }
            sql << kInsertTagNameSql, soci::use(created), soci::use(name);
            tag_id = std::move(created);
          }

          sql << kInsertImageTagSql, soci::use(image_id),
              soci::use(*tag_id);
        }
      }
    }

    transaction.commit();
    return saved;
  } catch (const std::exception &) {
    return TagimgRepositoryError::QueryFailed;
  }
}

constexpr const char *kArtworkColumnsSql = R"sql(
       a.id::text,
       a.title,
       a.description,
       to_char(a.published_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"'),
       to_char(a.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"'),
       to_char(a.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"')
)sql";

// 按相似度排序时的分数表达式：作品内所有已向量化图片的最高余弦相似度。
// 只写一次 :sort_embedding，SELECT 与 ORDER BY 都引用 sort_score 别名，
// 避免同一命名参数在一条语句里出现两次。
constexpr const char *kSimilarityScoreSql = R"sql(
       (SELECT max(1 - (c.embedding <=> CAST(:sort_embedding AS vector)))
        FROM artwork_images ai
        JOIN chinese_clip_vit_large_patch14 c ON c.image_id = ai.image_id
        WHERE ai.artwork_id = a.id))sql";

constexpr const char *kFindArtworkSql = R"sql(
SELECT
       a.id::text,
       a.title,
       a.description,
       to_char(a.published_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"'),
       to_char(a.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"'),
       to_char(a.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"')
FROM artworks a
WHERE a.id = CAST(:id AS uuid)
)sql";

// 作品查询的公共部分：参数 CTE 与过滤条件。
//
// 每个命名参数只出现一次：后面的过滤都引用 params，避免 SOCI 重复绑定。
constexpr const char *kArtworkSearchWithSql = R"sql(
WITH RECURSIVE params AS (
  SELECT CAST(:title AS text) AS title,
         CAST(:entity_ids AS uuid[]) AS entity_ids,
         CAST(:artist_ids AS uuid[]) AS artist_ids,
         CAST(:include_tags AS uuid[]) AS include_tags,
         CAST(:exclude_tags AS uuid[]) AS exclude_tags,
         CAST(:include_count AS bigint) AS include_count,
         CAST(:expand_include AS boolean) AS expand_include,
         CAST(:expand_exclude AS boolean) AS expand_exclude,
         CAST(:include_all AS boolean) AS include_all
),
include_scope(root_id, tag_id) AS (
  SELECT x.id, x.id
  FROM params p, unnest(p.include_tags) AS x(id)
  UNION ALL
  SELECT s.root_id, t.id
  FROM include_scope s
  JOIN tags t ON t.parent_id = s.tag_id
  CROSS JOIN params p
  WHERE p.expand_include
),
exclude_scope(root_id, tag_id) AS (
  SELECT x.id, x.id
  FROM params p, unnest(p.exclude_tags) AS x(id)
  UNION ALL
  SELECT s.root_id, t.id
  FROM exclude_scope s
  JOIN tags t ON t.parent_id = s.tag_id
  CROSS JOIN params p
  WHERE p.expand_exclude
)
)sql";

constexpr const char *kArtworkSearchFromSql = R"sql(
FROM artworks a
CROSS JOIN params p
WHERE (p.title IS NULL OR a.title ILIKE p.title ESCAPE '\')
  AND (p.entity_ids IS NULL OR EXISTS (
        SELECT 1
        FROM artwork_images ai
        JOIN images i ON i.id = ai.image_id
        WHERE ai.artwork_id = a.id AND i.entity_id = ANY(p.entity_ids)))
  AND (p.artist_ids IS NULL OR EXISTS (
        SELECT 1
        FROM artwork_artists aa
        WHERE aa.artwork_id = a.id AND aa.artist_id = ANY(p.artist_ids)))
  AND (p.include_tags IS NULL OR
       (SELECT count(DISTINCT s.root_id)
        FROM artwork_images ai
        JOIN image_tags it ON it.image_id = ai.image_id
        JOIN include_scope s ON s.tag_id = it.tag_id
        WHERE ai.artwork_id = a.id)
       >= CASE WHEN p.include_all THEN p.include_count ELSE 1 END)
  AND (p.exclude_tags IS NULL OR NOT EXISTS (
        SELECT 1
        FROM artwork_images ai
        JOIN image_tags it ON it.image_id = ai.image_id
        JOIN exclude_scope s ON s.tag_id = it.tag_id
        WHERE ai.artwork_id = a.id))
)sql";

constexpr const char *kImageSearchWithSql = R"sql(
WITH RECURSIVE params AS (
  SELECT CAST(:entity_ids AS uuid[]) AS entity_ids,
         CAST(:image_ids AS uuid[]) AS image_ids,
         CAST(:artist_ids AS uuid[]) AS artist_ids,
         CAST(:include_tags AS uuid[]) AS include_tags,
         CAST(:exclude_tags AS uuid[]) AS exclude_tags,
         CAST(:include_count AS bigint) AS include_count,
         CAST(:expand_include AS boolean) AS expand_include,
         CAST(:expand_exclude AS boolean) AS expand_exclude,
         CAST(:include_all AS boolean) AS include_all,
         CAST(:reference_image_id AS uuid) AS reference_image_id
),
include_scope(root_id, tag_id) AS (
  SELECT x.id, x.id
  FROM params p, unnest(p.include_tags) AS x(id)
  UNION ALL
  SELECT s.root_id, t.id
  FROM include_scope s
  JOIN tags t ON t.parent_id = s.tag_id
  CROSS JOIN params p
  WHERE p.expand_include
),
exclude_scope(root_id, tag_id) AS (
  SELECT x.id, x.id
  FROM params p, unnest(p.exclude_tags) AS x(id)
  UNION ALL
  SELECT s.root_id, t.id
  FROM exclude_scope s
  JOIN tags t ON t.parent_id = s.tag_id
  CROSS JOIN params p
  WHERE p.expand_exclude
)
)sql";

constexpr const char *kImageSearchFromSql = R"sql(
FROM images i
CROSS JOIN params p
WHERE (p.entity_ids IS NULL OR i.entity_id = ANY(p.entity_ids))
  AND (p.image_ids IS NULL OR i.id = ANY(p.image_ids))
  AND (p.artist_ids IS NULL OR EXISTS (
        SELECT 1
        FROM artwork_images ai
        JOIN artwork_artists aa ON aa.artwork_id = ai.artwork_id
        WHERE ai.image_id = i.id AND aa.artist_id = ANY(p.artist_ids)))
  AND (p.include_tags IS NULL OR
       (SELECT count(DISTINCT s.root_id)
        FROM image_tags it
        JOIN include_scope s ON s.tag_id = it.tag_id
        WHERE it.image_id = i.id)
       >= CASE WHEN p.include_all THEN p.include_count ELSE 1 END)
  AND (p.exclude_tags IS NULL OR NOT EXISTS (
        SELECT 1
        FROM image_tags it
        JOIN exclude_scope s ON s.tag_id = it.tag_id
        WHERE it.image_id = i.id))
  AND (p.reference_image_id IS NULL OR i.id <> p.reference_image_id)
  AND (p.reference_image_id IS NULL OR EXISTS (
        SELECT 1
        FROM chinese_clip_vit_large_patch14 c
        WHERE c.image_id = i.id))
)sql";

constexpr const char *kImageColumnsSql = R"sql(
       i.id::text,
       i.width,
       i.height,
       i.frame_count,
       to_char(i.created_at AT TIME ZONE 'UTC',
               'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at,
       CASE WHEN i.scan_at IS NULL THEN NULL
            ELSE to_char(i.scan_at AT TIME ZONE 'UTC',
                         'YYYY-MM-DD"T"HH24:MI:SS"Z"') END AS scan_at,
       to_char((SELECT max(a.published_at)
                FROM artwork_images ai
                JOIN artworks a ON a.id = ai.artwork_id
                WHERE ai.image_id = i.id) AT TIME ZONE 'UTC',
               'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS published_at
)sql";

constexpr const char *kImageSimilarityScoreSql = R"sql(
       (SELECT 1 - (c.embedding <=> CAST(:sort_embedding AS vector))
        FROM chinese_clip_vit_large_patch14 c
        WHERE c.image_id = i.id) AS sort_score
)sql";

constexpr const char *kArtworkImagesSql = R"sql(
SELECT ai.artwork_id::text, ai.image_id::text
FROM artwork_images ai
WHERE ai.artwork_id = ANY(CAST(:ids AS uuid[]))
ORDER BY ai.artwork_id, ai.position
)sql";

constexpr const char *kArtworkTagsSql = R"sql(
SELECT DISTINCT ai.artwork_id::text, it.tag_id::text
FROM artwork_images ai
JOIN image_tags it ON it.image_id = ai.image_id
WHERE ai.artwork_id = ANY(CAST(:ids AS uuid[]))
ORDER BY ai.artwork_id::text, it.tag_id::text
)sql";

constexpr const char *kImageTagsSql = R"sql(
SELECT it.image_id::text, it.tag_id::text
FROM image_tags it
WHERE it.image_id = ANY(CAST(:ids AS uuid[]))
ORDER BY it.image_id, it.tag_id
)sql";

constexpr const char *kImageArtworksSql = R"sql(
SELECT ai.image_id::text,
       a.id::text,
       COALESCE(a.title, '') AS title,
       to_char(a.published_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"')
FROM artwork_images ai
JOIN artworks a ON a.id = ai.artwork_id
WHERE ai.image_id = ANY(CAST(:ids AS uuid[]))
ORDER BY ai.image_id,
         a.published_at DESC NULLS LAST,
         a.created_at DESC,
         a.id
)sql";

// 从标签向上回溯，得到「根::...::自身」的路径。
// distance = 0 是标签自身，1 是直接父标签，依次递增。
constexpr const char *kTagInfoSql = R"sql(
WITH RECURSIVE ancestors(tag_id, cur_id, distance) AS (
  SELECT t.id, t.id, 0
  FROM tags t
  WHERE t.id = ANY(CAST(:tag_ids AS uuid[]))
  UNION ALL
  SELECT a.tag_id, p.parent_id, a.distance + 1
  FROM ancestors a
  JOIN tags p ON p.id = a.cur_id
  WHERE p.parent_id IS NOT NULL
),
segments AS (
  SELECT a.tag_id,
         a.distance,
         COALESCE(n.name, a.cur_id::text) AS segment
  FROM ancestors a
  LEFT JOIN LATERAL (
    SELECT name
    FROM tag_name
    WHERE tag_id = a.cur_id AND type = 'standard'
    ORDER BY id
    LIMIT 1
  ) n ON TRUE
),
aggregated AS (
  SELECT tag_id,
         string_agg(segment, '::' ORDER BY distance DESC) AS path,
         array_agg(segment ORDER BY distance) AS parts,
         max(distance) AS depth
  FROM segments
  GROUP BY tag_id
)
SELECT a.tag_id::text,
       COALESCE(n.name, a.tag_id::text) AS name,
       CASE WHEN a.depth >= 1 THEN a.parts[2] ELSE NULL END AS category,
       a.path,
       t.parent_id::text,
       (EXISTS (
         SELECT 1 FROM tags child WHERE child.parent_id = t.id
       ))::int AS has_children
FROM aggregated a
JOIN tags t ON t.id = a.tag_id
LEFT JOIN LATERAL (
  SELECT name
  FROM tag_name
  WHERE tag_id = a.tag_id AND type = 'standard'
  ORDER BY id
  LIMIT 1
) n ON TRUE
)sql";

constexpr const char *kArtworkAliasesSql = R"sql(
SELECT tn.tag_id::text, tn.name
FROM tag_name tn
WHERE tn.type = 'alias' AND tn.tag_id = ANY(CAST(:tag_ids AS uuid[]))
ORDER BY tn.tag_id, tn.name
)sql";

// 按父标签列出直接子标签；parent_id 为空时列出顶层标签。
constexpr const char *kTagChildrenSql = R"sql(
SELECT t.id::text
FROM tags t
LEFT JOIN LATERAL (
  SELECT name
  FROM tag_name
  WHERE tag_id = t.id AND type = 'standard'
  ORDER BY id
  LIMIT 1
) n ON TRUE
WHERE t.parent_id IS NOT DISTINCT FROM CAST(:parent_id AS uuid)
ORDER BY lower(COALESCE(n.name, t.id::text)),
         COALESCE(n.name, t.id::text),
         t.id
)sql";

// 标签树搜索：标准名和别名都参与匹配，每个标签只取最佳命中。
constexpr const char *kTagTreeMatchSql = R"sql(
WITH matched AS (
  SELECT t.id AS tag_id,
         n.name,
         (n.type = 'standard')::int AS is_standard,
         (lower(n.name) = lower(:text)) AS is_exact,
         (n.name ILIKE :prefix ESCAPE '\') AS is_prefix
  FROM tags t
  JOIN tag_name n ON n.tag_id = t.id AND n.type IN ('standard', 'alias')
  WHERE n.name ILIKE :pattern ESCAPE '\'
),
best AS (
  SELECT DISTINCT ON (tag_id)
         tag_id, name, is_standard, is_exact, is_prefix
  FROM matched
  ORDER BY tag_id, is_exact DESC, is_prefix DESC, length(name), name,
           is_standard DESC
)
SELECT tag_id::text, name, is_standard
FROM best
ORDER BY is_exact DESC, is_prefix DESC, length(name), name, tag_id
)sql";

// 为搜索命中的标签取完整祖先链；distance=0 是命中节点自身。
constexpr const char *kTagTreeAncestorsSql = R"sql(
WITH RECURSIVE chains(match_id, cur_id, distance) AS (
  SELECT t.id, t.id, 0
  FROM tags t
  WHERE t.id = ANY(CAST(:tag_ids AS uuid[]))
  UNION ALL
  SELECT c.match_id, p.parent_id, c.distance + 1
  FROM chains c
  JOIN tags p ON p.id = c.cur_id
  WHERE p.parent_id IS NOT NULL
)
SELECT match_id::text, cur_id::text, distance
FROM chains
ORDER BY match_id, distance DESC
)sql";

// 批量读取标签完整信息（含父级、路径、别名、是否有子标签）。
std::map<std::string, ArtworkTagInfo>
load_tag_infos(soci::session &sql, const std::vector<std::string> &tag_ids) {
  std::map<std::string, ArtworkTagInfo> infos;
  if (tag_ids.empty()) {
    return infos;
  }

  const std::string tag_array = uuid_array_literal(tag_ids);
  soci::rowset<soci::row> rows =
      (sql.prepare << kTagInfoSql, soci::use(tag_array));
  for (const soci::row &row : rows) {
    ArtworkTagInfo item;
    item.id = row.get<std::string>(0);
    item.name = row.get<std::string>(1);
    if (row.get_indicator(2) != soci::i_null) {
      item.category = row.get<std::string>(2);
    }
    item.path = row.get<std::string>(3);
    if (row.get_indicator(4) != soci::i_null) {
      item.parent_id = row.get<std::string>(4);
    }
    item.has_children = row.get<int>(5) != 0;
    infos.emplace(item.id, std::move(item));
  }

  soci::rowset<soci::row> aliases =
      (sql.prepare << kArtworkAliasesSql, soci::use(tag_array));
  for (const soci::row &row : aliases) {
    auto found = infos.find(row.get<std::string>(0));
    if (found != infos.end()) {
      found->second.aliases.push_back(row.get<std::string>(1));
    }
  }
  return infos;
}

// 读取单个标签的完整信息（含父级、路径、别名）。
std::optional<ArtworkTagInfo> load_tag_info(soci::session &sql,
                                            const std::string &tag_id) {
  auto infos = load_tag_infos(sql, {tag_id});
  auto found = infos.find(tag_id);
  if (found == infos.end()) {
    return std::nullopt;
  }
  return std::move(found->second);
}

std::string tag_lock_key(const std::optional<std::string> &parent_id) {
  return parent_id ? "tag:" + *parent_id : "root";
}

void acquire_tag_lock(soci::session &sql, const std::string &key) {
  soci::rowset<soci::row> rows =
      (sql.prepare << kAcquireTagLockSql, soci::use(key));
  for (const soci::row &row : rows) {
    (void)row;
    break;
  }
}

std::optional<std::pair<std::string, std::string>>
find_standard_name(soci::session &sql, const std::string &tag_id) {
  soci::rowset<soci::row> rows =
      (sql.prepare << kSelectStandardNameSql, soci::use(tag_id));
  const auto row = rows.begin();
  if (row == rows.end()) {
    return std::nullopt;
  }
  return std::make_pair(row->get<std::string>(0), row->get<std::string>(1));
}

constexpr const char *kArtworkArtistsSql = R"sql(
SELECT aa.artwork_id::text, ar.id::text, ar.name
FROM artwork_artists aa
JOIN artists ar ON ar.id = aa.artist_id
WHERE aa.artwork_id = ANY(CAST(:ids AS uuid[]))
ORDER BY aa.artwork_id, ar.name
)sql";

constexpr const char *kCountTagsSql = R"sql(
SELECT count(*)
FROM tags
WHERE id = ANY(CAST(:ids AS uuid[]))
)sql";

ArtworkSummary parse_artwork_row(const soci::row &row) {
  ArtworkSummary item;
  item.id = row.get<std::string>(0);
  item.title = row.get<std::string>(1);
  if (row.get_indicator(2) != soci::i_null) {
    item.description = row.get<std::string>(2);
  }
  if (row.get_indicator(3) != soci::i_null) {
    item.published_at = row.get<std::string>(3);
  }
  item.created_at = row.get<std::string>(4);
  item.updated_at = row.get<std::string>(5);
  return item;
}

ImageSummary parse_image_row(const soci::row &row) {
  ImageSummary item;
  item.id = row.get<std::string>(0);
  if (row.get_indicator(1) != soci::i_null) {
    item.width = row.get<int>(1);
  }
  if (row.get_indicator(2) != soci::i_null) {
    item.height = row.get<int>(2);
  }
  if (row.get_indicator(3) != soci::i_null) {
    item.frame_count = row.get<int>(3);
  }
  item.created_at = row.get<std::string>(4);
  if (row.get_indicator(5) != soci::i_null) {
    item.scan_at = row.get<std::string>(5);
  }
  if (row.get_indicator(6) != soci::i_null) {
    item.published_at = row.get<std::string>(6);
  }
  return item;
}

// 一次性补齐一批作品的图片、标签、艺术家。
void attach_relations(soci::session &sql, std::vector<ArtworkSummary> &items) {
  if (items.empty()) {
    return;
  }

  std::vector<std::string> artwork_ids;
  artwork_ids.reserve(items.size());
  std::map<std::string, std::size_t> index;
  for (std::size_t i = 0; i < items.size(); ++i) {
    artwork_ids.push_back(items[i].id);
    index.emplace(items[i].id, i);
  }
  const std::string artwork_array = uuid_array_literal(artwork_ids);

  soci::rowset<soci::row> images =
      (sql.prepare << kArtworkImagesSql, soci::use(artwork_array));
  for (const soci::row &row : images) {
    const auto found = index.find(row.get<std::string>(0));
    if (found == index.end()) {
      continue;
    }
    ArtworkSummary &item = items[found->second];
    item.image_ids.push_back(row.get<std::string>(1));
  }

  std::vector<std::pair<std::size_t, std::string>> tag_links;
  std::set<std::string> tag_ids;
  soci::rowset<soci::row> tags =
      (sql.prepare << kArtworkTagsSql, soci::use(artwork_array));
  for (const soci::row &row : tags) {
    const auto found = index.find(row.get<std::string>(0));
    if (found == index.end()) {
      continue;
    }
    std::string tag_id = row.get<std::string>(1);
    tag_ids.insert(tag_id);
    tag_links.emplace_back(found->second, std::move(tag_id));
  }

  std::map<std::string, ArtworkTagInfo> tag_infos;
  if (!tag_ids.empty()) {
    const std::vector<std::string> ids(tag_ids.begin(), tag_ids.end());
    const std::string tag_array = uuid_array_literal(ids);
    soci::rowset<soci::row> rows =
        (sql.prepare << kTagInfoSql, soci::use(tag_array));
    for (const soci::row &row : rows) {
      ArtworkTagInfo info;
      info.id = row.get<std::string>(0);
      info.name = row.get<std::string>(1);
      if (row.get_indicator(2) != soci::i_null) {
        info.category = row.get<std::string>(2);
      }
      info.path = row.get<std::string>(3);
      if (row.get_indicator(4) != soci::i_null) {
        info.parent_id = row.get<std::string>(4);
      }
      tag_infos.emplace(info.id, std::move(info));
    }

    // 别名单独查一次，避免在路径 CTE 里放大结果集。
    soci::rowset<soci::row> aliases =
        (sql.prepare << kArtworkAliasesSql, soci::use(tag_array));
    for (const soci::row &row : aliases) {
      const auto found = tag_infos.find(row.get<std::string>(0));
      if (found != tag_infos.end()) {
        found->second.aliases.push_back(row.get<std::string>(1));
      }
    }
  }
  for (const auto &[position, tag_id] : tag_links) {
    const auto found = tag_infos.find(tag_id);
    if (found != tag_infos.end()) {
      items[position].tags.push_back(found->second);
    }
  }

  soci::rowset<soci::row> artists =
      (sql.prepare << kArtworkArtistsSql, soci::use(artwork_array));
  for (const soci::row &row : artists) {
    const auto found = index.find(row.get<std::string>(0));
    if (found == index.end()) {
      continue;
    }
    ArtworkSummary &item = items[found->second];
    item.artist_ids.push_back(row.get<std::string>(1));
    item.artists.push_back(
        ArtworkArtistInfo{row.get<std::string>(1), row.get<std::string>(2)});
  }

  for (ArtworkSummary &item : items) {
    std::sort(item.tags.begin(), item.tags.end(),
              [](const ArtworkTagInfo &left, const ArtworkTagInfo &right) {
                return left.name < right.name ||
                       (left.name == right.name && left.id < right.id);
              });
    item.tag_ids.clear();
    item.tag_ids.reserve(item.tags.size());
    for (const ArtworkTagInfo &tag : item.tags) {
      item.tag_ids.push_back(tag.id);
    }
  }
}

// 一次性补齐一批图片的标签与所属作品。
void attach_image_relations(soci::session &sql,
                            std::vector<ImageSummary> &items) {
  if (items.empty()) {
    return;
  }

  std::vector<std::string> image_ids;
  image_ids.reserve(items.size());
  std::map<std::string, std::size_t> index;
  for (std::size_t i = 0; i < items.size(); ++i) {
    image_ids.push_back(items[i].id);
    index.emplace(items[i].id, i);
  }
  const std::string image_array = uuid_array_literal(image_ids);

  std::vector<std::pair<std::size_t, std::string>> tag_links;
  std::set<std::string> tag_ids;
  soci::rowset<soci::row> tags =
      (sql.prepare << kImageTagsSql, soci::use(image_array));
  for (const soci::row &row : tags) {
    const auto found = index.find(row.get<std::string>(0));
    if (found == index.end()) {
      continue;
    }
    std::string tag_id = row.get<std::string>(1);
    tag_ids.insert(tag_id);
    tag_links.emplace_back(found->second, std::move(tag_id));
  }

  std::map<std::string, ArtworkTagInfo> tag_infos;
  if (!tag_ids.empty()) {
    const std::vector<std::string> ids(tag_ids.begin(), tag_ids.end());
    const std::string tag_array = uuid_array_literal(ids);
    soci::rowset<soci::row> rows =
        (sql.prepare << kTagInfoSql, soci::use(tag_array));
    for (const soci::row &row : rows) {
      ArtworkTagInfo info;
      info.id = row.get<std::string>(0);
      info.name = row.get<std::string>(1);
      if (row.get_indicator(2) != soci::i_null) {
        info.category = row.get<std::string>(2);
      }
      info.path = row.get<std::string>(3);
      if (row.get_indicator(4) != soci::i_null) {
        info.parent_id = row.get<std::string>(4);
      }
      tag_infos.emplace(info.id, std::move(info));
    }

    soci::rowset<soci::row> aliases =
        (sql.prepare << kArtworkAliasesSql, soci::use(tag_array));
    for (const soci::row &row : aliases) {
      const auto found = tag_infos.find(row.get<std::string>(0));
      if (found != tag_infos.end()) {
        found->second.aliases.push_back(row.get<std::string>(1));
      }
    }
  }
  for (const auto &[position, tag_id] : tag_links) {
    const auto found = tag_infos.find(tag_id);
    if (found != tag_infos.end()) {
      items[position].tags.push_back(found->second);
    }
  }

  soci::rowset<soci::row> artworks =
      (sql.prepare << kImageArtworksSql, soci::use(image_array));
  for (const soci::row &row : artworks) {
    const auto found = index.find(row.get<std::string>(0));
    if (found == index.end()) {
      continue;
    }
    ImageArtworkInfo info;
    info.id = row.get<std::string>(1);
    info.title = row.get<std::string>(2);
    if (row.get_indicator(3) != soci::i_null) {
      info.published_at = row.get<std::string>(3);
    }
    items[found->second].artworks.push_back(std::move(info));
  }

  for (ImageSummary &item : items) {
    std::sort(item.tags.begin(), item.tags.end(),
              [](const ArtworkTagInfo &left, const ArtworkTagInfo &right) {
                return left.name < right.name ||
                       (left.name == right.name && left.id < right.id);
              });
    item.tag_ids.clear();
    item.tag_ids.reserve(item.tags.size());
    for (const ArtworkTagInfo &tag : item.tags) {
      item.tag_ids.push_back(tag.id);
    }
  }
}

} // namespace

std::string_view to_string(TagimgRepositoryError error) noexcept {
  switch (error) {
  case TagimgRepositoryError::InvalidArgument:
    return "InvalidArgument";
  case TagimgRepositoryError::ConnectionFailed:
    return "ConnectionFailed";
  case TagimgRepositoryError::DuplicateImage:
    return "DuplicateImage";
  case TagimgRepositoryError::UnknownTag:
    return "UnknownTag";
  case TagimgRepositoryError::UnknownArtist:
    return "UnknownArtist";
  case TagimgRepositoryError::UnknownImage:
    return "UnknownImage";
  case TagimgRepositoryError::TagHierarchyMissing:
    return "TagHierarchyMissing";
  case TagimgRepositoryError::TagNameConflict:
    return "TagNameConflict";
  case TagimgRepositoryError::TagParentNotFound:
    return "TagParentNotFound";
  case TagimgRepositoryError::TagCycle:
    return "TagCycle";
  case TagimgRepositoryError::QueryFailed:
    return "QueryFailed";
  }
  return "Unknown";
}

struct TagimgRepository::Impl {
  TagimgDbConfig config;
  std::unique_ptr<soci::connection_pool> pool;
};

TagimgRepository::TagimgRepository(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {
}

TagimgRepository::~TagimgRepository() = default;

Result<std::unique_ptr<TagimgRepository>, TagimgRepositoryError>
TagimgRepository::connect(TagimgDbConfig config, std::size_t pool_size) {
  if (pool_size == 0) {
    pool_size = 1;
  }

  auto impl = std::make_unique<Impl>();
  impl->config = std::move(config);
  const std::string conninfo = db_detail::make_conninfo(
      impl->config.host, impl->config.port, impl->config.dbname,
      impl->config.user, impl->config.password);

  try {
    impl->pool = std::make_unique<soci::connection_pool>(pool_size);
    // 先开一个连接做快速失败：数据库不可达时立刻返回，
    // 不必等每个池槽位都耗尽 connect_timeout。
    impl->pool->at(0).open(soci::postgresql, conninfo);
    impl->pool->at(0).once << "SELECT 1";
    for (std::size_t i = 1; i < pool_size; ++i) {
      impl->pool->at(i).open(soci::postgresql, conninfo);
    }
  } catch (const std::exception &) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  return std::unique_ptr<TagimgRepository>(
      new TagimgRepository(std::move(impl)));
}

Result<SavedArtwork, TagimgRepositoryError>
TagimgRepository::save_artwork(const ArtworkInput &input) const {
  if (input.title.empty() || input.entity_ids.empty()) {
    return TagimgRepositoryError::InvalidArgument;
  }
  for (const std::string &id : input.entity_ids) {
    if (!is_uuid(id)) {
      return TagimgRepositoryError::InvalidArgument;
    }
  }
  for (const std::string &id : input.tag_ids) {
    if (!is_uuid(id)) {
      return TagimgRepositoryError::InvalidArgument;
    }
  }
  for (const std::string &id : input.artist_ids) {
    if (!is_uuid(id)) {
      return TagimgRepositoryError::InvalidArgument;
    }
  }
  for (const auto &image_tags : input.scanned_tags) {
    for (const ArtworkTag &tag : image_tags) {
      if (tag.name.empty() || tag.category.empty()) {
        return TagimgRepositoryError::InvalidArgument;
      }
    }
  }

  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  ArtworkWriteInput write;
  write.title = input.title;
  write.description = input.description;
  write.published_at = input.published_at;
  write.tag_ids = input.tag_ids;
  write.artist_ids = input.artist_ids;
  write.scanned_tags = input.scanned_tags;
  write.embeddings = input.embeddings;
  write.metadata = input.metadata;

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());
    return write_artwork(sql, write, input.entity_ids, {}, false);
  } catch (const std::exception &) {
    return TagimgRepositoryError::QueryFailed;
  }
}

Result<SavedArtwork, TagimgRepositoryError>
TagimgRepository::save_artwork_from_images(
    const ImageArtworkInput &input) const {
  if (input.title.empty() || input.image_ids.empty()) {
    return TagimgRepositoryError::InvalidArgument;
  }

  const std::set<std::string> unique(input.image_ids.begin(),
                                     input.image_ids.end());
  if (unique.size() != input.image_ids.size()) {
    return TagimgRepositoryError::InvalidArgument;
  }
  for (const std::string &id : input.image_ids) {
    if (!is_uuid(id)) {
      return TagimgRepositoryError::InvalidArgument;
    }
  }
  for (const std::string &id : input.tag_ids) {
    if (!is_uuid(id)) {
      return TagimgRepositoryError::InvalidArgument;
    }
  }
  for (const std::string &id : input.artist_ids) {
    if (!is_uuid(id)) {
      return TagimgRepositoryError::InvalidArgument;
    }
  }
  for (const auto &image_tags : input.scanned_tags) {
    for (const ArtworkTag &tag : image_tags) {
      if (tag.name.empty() || tag.category.empty()) {
        return TagimgRepositoryError::InvalidArgument;
      }
    }
  }

  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  ArtworkWriteInput write;
  write.title = input.title;
  write.description = input.description;
  write.published_at = input.published_at;
  write.tag_ids = input.tag_ids;
  write.artist_ids = input.artist_ids;
  write.scanned_tags = input.scanned_tags;
  write.embeddings = input.embeddings;
  write.metadata = input.metadata;

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());
    return write_artwork(sql, write, {}, input.image_ids, input.rescan);
  } catch (const std::exception &) {
    return TagimgRepositoryError::QueryFailed;
  }
}

Result<bool, TagimgRepositoryError> TagimgRepository::image_exists_by_entity_id(
    const std::string &entity_id) const {
  if (!is_uuid(entity_id)) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());
    return count_rows(sql, kCountImageSql, entity_id) > 0;
  } catch (const std::exception &) {
    return TagimgRepositoryError::QueryFailed;
  }
}

Result<std::optional<std::string>, TagimgRepositoryError>
TagimgRepository::find_image_entity_id(const std::string &image_id) const {
  if (!is_uuid(image_id)) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());
    soci::rowset<soci::row> rows =
        (sql.prepare << kFindImageEntitySql, soci::use(image_id));
    const auto row = rows.begin();
    if (row == rows.end() || row->get_indicator(0) == soci::i_null) {
      return std::optional<std::string>{};
    }
    return std::optional<std::string>(row->get<std::string>(0));
  } catch (const std::exception &) {
    return TagimgRepositoryError::QueryFailed;
  }
}

Result<std::optional<ArtworkSummary>, TagimgRepositoryError>
TagimgRepository::find_artwork(const std::string &artwork_id) const {
  if (!is_uuid(artwork_id)) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());

    std::vector<ArtworkSummary> items;
    soci::rowset<soci::row> rows =
        (sql.prepare << kFindArtworkSql, soci::use(artwork_id));
    for (const soci::row &row : rows) {
      items.push_back(parse_artwork_row(row));
    }
    attach_relations(sql, items);

    if (items.empty()) {
      return std::optional<ArtworkSummary>{};
    }
    return std::optional<ArtworkSummary>(std::move(items.front()));
  } catch (const std::exception &) {
    return TagimgRepositoryError::QueryFailed;
  }
}

Result<ArtworkSearchResult, TagimgRepositoryError>
TagimgRepository::search_artworks(const ArtworkSearchQuery &query) const {
  if (query.limit == 0 || query.limit > 100 || query.offset > 10000) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (query.title && utf8_length(*query.title) > kMaxTitleLength) {
    return TagimgRepositoryError::InvalidArgument;
  }
  // 按相似度排序必须有 768 维向量；反过来传了向量却不是相似度排序也拒绝。
  if (query.order_by == ArtworkSortField::Similarity) {
    if (query.sort_embedding.size() != kClipEmbeddingDim) {
      return TagimgRepositoryError::InvalidArgument;
    }
    for (float value : query.sort_embedding) {
      if (!std::isfinite(value)) {
        return TagimgRepositoryError::InvalidArgument;
      }
    }
  } else if (!query.sort_embedding.empty()) {
    return TagimgRepositoryError::InvalidArgument;
  }
  for (const auto *ids : {&query.entity_ids, &query.artist_ids,
                          &query.include_tag_ids, &query.exclude_tag_ids}) {
    for (const std::string &id : *ids) {
      if (!is_uuid(id)) {
        return TagimgRepositoryError::InvalidArgument;
      }
    }
  }
  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  const std::vector<std::string> include_ids =
      unique_ids(query.include_tag_ids);
  const std::vector<std::string> exclude_ids =
      unique_ids(query.exclude_tag_ids);

  std::string title_value;
  soci::indicator title_indicator = soci::i_null;
  if (query.title && !query.title->empty()) {
    title_value = like_pattern(*query.title);
    title_indicator = soci::i_ok;
  }
  std::string entity_value;
  soci::indicator entity_indicator = soci::i_null;
  if (!query.entity_ids.empty()) {
    entity_value = uuid_array_literal(query.entity_ids);
    entity_indicator = soci::i_ok;
  }
  std::string artist_value;
  soci::indicator artist_indicator = soci::i_null;
  if (!query.artist_ids.empty()) {
    artist_value = uuid_array_literal(query.artist_ids);
    artist_indicator = soci::i_ok;
  }
  std::string include_value;
  soci::indicator include_indicator = soci::i_null;
  if (!include_ids.empty()) {
    include_value = uuid_array_literal(include_ids);
    include_indicator = soci::i_ok;
  }
  std::string exclude_value;
  soci::indicator exclude_indicator = soci::i_null;
  if (!exclude_ids.empty()) {
    exclude_value = uuid_array_literal(exclude_ids);
    exclude_indicator = soci::i_ok;
  }

  const long long include_count = static_cast<long long>(include_ids.size());
  const int expand_include = query.expand_include_tag_descendants ? 1 : 0;
  const int expand_exclude = query.expand_exclude_tag_descendants ? 1 : 0;
  const int include_all = query.include_all_tags ? 1 : 0;
  // 多取一行用于判断 has_more，取回后再把多出的那条丢掉。
  const int limit_value = static_cast<int>(query.limit) + 1;
  const int offset_value = static_cast<int>(query.offset);

  const std::string direction = query.descending ? "DESC" : "ASC";
  std::string order_sql = "\nORDER BY ";
  switch (query.order_by) {
  case ArtworkSortField::CreatedAt:
    order_sql += "a.created_at " + direction + ", a.id " + direction;
    break;
  case ArtworkSortField::PublishedAt:
    order_sql +=
        "a.published_at " + direction + " NULLS LAST, a.id " + direction;
    break;
  case ArtworkSortField::Title:
    order_sql += "a.title " + direction + ", a.id " + direction;
    break;
  case ArtworkSortField::Similarity:
    // 每部作品取所有已向量化图片的最高余弦相似度；没有向量的作品排最后。
    // 相似度越高越靠前，所以方向固定为 DESC。
    order_sql += "sort_score DESC NULLS LAST, a.id DESC";
    break;
  }

  // 相似度这一路多取一列分数，供前端显示；其它排序方式不返回分数。
  const bool with_score = query.order_by == ArtworkSortField::Similarity;
  const std::string page_sql =
      std::string(kArtworkSearchWithSql) + "\nSELECT " + kArtworkColumnsSql +
      (with_score ? std::string(", ") + kSimilarityScoreSql +
                        " AS sort_score"
                  : std::string()) +
      kArtworkSearchFromSql + order_sql + "\nLIMIT :limit OFFSET :offset";
  const std::string count_sql = std::string(kArtworkSearchWithSql) +
                                "\nSELECT count(*)" + kArtworkSearchFromSql;

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());

    // 命中任一标签即存在的检查；缺任何一个都按 UnknownTag 处理。
    std::vector<std::string> checked_ids = include_ids;
    checked_ids.insert(checked_ids.end(), exclude_ids.begin(),
                       exclude_ids.end());
    checked_ids = unique_ids(checked_ids);
    if (!checked_ids.empty()) {
      long long found = 0;
      const std::string checked_array = uuid_array_literal(checked_ids);
      sql << kCountTagsSql, soci::use(checked_array), soci::into(found);
      if (found != static_cast<long long>(checked_ids.size())) {
        return TagimgRepositoryError::UnknownTag;
      }
    }

    ArtworkSearchResult result;
    if (query.order_by == ArtworkSortField::Similarity) {
      const std::string embedding_value = pgvector_literal(query.sort_embedding);
      soci::rowset<soci::row> rows =
          (sql.prepare << page_sql, soci::use(title_value, title_indicator),
           soci::use(entity_value, entity_indicator),
           soci::use(artist_value, artist_indicator),
           soci::use(include_value, include_indicator),
           soci::use(exclude_value, exclude_indicator),
           soci::use(include_count), soci::use(expand_include),
           soci::use(expand_exclude), soci::use(include_all),
           soci::use(embedding_value), soci::use(limit_value),
           soci::use(offset_value));
      for (const soci::row &row : rows) {
        ArtworkSummary item = parse_artwork_row(row);
        // 第 7 列是相似度分数；作品没有向量时是 NULL。
        if (row.get_indicator(6) != soci::i_null) {
          item.sort_score = row.get<double>(6);
        }
        result.items.push_back(std::move(item));
      }
    } else {
      soci::rowset<soci::row> rows =
          (sql.prepare << page_sql, soci::use(title_value, title_indicator),
           soci::use(entity_value, entity_indicator),
           soci::use(artist_value, artist_indicator),
           soci::use(include_value, include_indicator),
           soci::use(exclude_value, exclude_indicator),
           soci::use(include_count), soci::use(expand_include),
           soci::use(expand_exclude), soci::use(include_all),
           soci::use(limit_value), soci::use(offset_value));
      for (const soci::row &row : rows) {
        result.items.push_back(parse_artwork_row(row));
      }
    }
    if (result.items.size() > query.limit) {
      result.items.pop_back();
      result.has_more = true;
    }
    attach_relations(sql, result.items);

    if (query.include_total) {
      long long total = 0;
      sql << count_sql, soci::use(title_value, title_indicator),
          soci::use(entity_value, entity_indicator),
          soci::use(artist_value, artist_indicator),
          soci::use(include_value, include_indicator),
          soci::use(exclude_value, exclude_indicator), soci::use(include_count),
          soci::use(expand_include), soci::use(expand_exclude),
          soci::use(include_all), soci::into(total);
      result.total = static_cast<std::size_t>(total);
    }

    return result;
  } catch (const std::exception &) {
    return TagimgRepositoryError::QueryFailed;
  }
}

Result<ImageSearchResult, TagimgRepositoryError>
TagimgRepository::search_images(const ImageSearchQuery &query) const {
  if (query.limit == 0 || query.limit > 100 || query.offset > 10000) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (query.order_by == ImageSortField::Similarity) {
    if (query.sort_embedding.size() != kClipEmbeddingDim) {
      return TagimgRepositoryError::InvalidArgument;
    }
    for (float value : query.sort_embedding) {
      if (!std::isfinite(value)) {
        return TagimgRepositoryError::InvalidArgument;
      }
    }
    if (query.reference_image_id &&
        !is_uuid(*query.reference_image_id)) {
      return TagimgRepositoryError::InvalidArgument;
    }
  } else if (!query.sort_embedding.empty() || query.reference_image_id) {
    return TagimgRepositoryError::InvalidArgument;
  }
  for (const auto *ids : {&query.entity_ids, &query.image_ids,
                          &query.artist_ids, &query.include_tag_ids,
                          &query.exclude_tag_ids}) {
    for (const std::string &id : *ids) {
      if (!is_uuid(id)) {
        return TagimgRepositoryError::InvalidArgument;
      }
    }
  }
  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  const std::vector<std::string> include_ids =
      unique_ids(query.include_tag_ids);
  const std::vector<std::string> exclude_ids =
      unique_ids(query.exclude_tag_ids);

  std::string entity_value;
  soci::indicator entity_indicator = soci::i_null;
  if (!query.entity_ids.empty()) {
    entity_value = uuid_array_literal(query.entity_ids);
    entity_indicator = soci::i_ok;
  }
  std::string image_value;
  soci::indicator image_indicator = soci::i_null;
  if (!query.image_ids.empty()) {
    image_value = uuid_array_literal(query.image_ids);
    image_indicator = soci::i_ok;
  }
  std::string artist_value;
  soci::indicator artist_indicator = soci::i_null;
  if (!query.artist_ids.empty()) {
    artist_value = uuid_array_literal(query.artist_ids);
    artist_indicator = soci::i_ok;
  }
  std::string include_value;
  soci::indicator include_indicator = soci::i_null;
  if (!include_ids.empty()) {
    include_value = uuid_array_literal(include_ids);
    include_indicator = soci::i_ok;
  }
  std::string exclude_value;
  soci::indicator exclude_indicator = soci::i_null;
  if (!exclude_ids.empty()) {
    exclude_value = uuid_array_literal(exclude_ids);
    exclude_indicator = soci::i_ok;
  }
  std::string reference_value;
  soci::indicator reference_indicator = soci::i_null;
  if (query.reference_image_id) {
    reference_value = *query.reference_image_id;
    reference_indicator = soci::i_ok;
  }

  const long long include_count = static_cast<long long>(include_ids.size());
  const int expand_include = query.expand_include_tag_descendants ? 1 : 0;
  const int expand_exclude = query.expand_exclude_tag_descendants ? 1 : 0;
  const int include_all = query.include_all_tags ? 1 : 0;
  const int limit_value = static_cast<int>(query.limit) + 1;
  const int offset_value = static_cast<int>(query.offset);

  const std::string direction = query.descending ? "DESC" : "ASC";
  std::string order_sql = "\nORDER BY ";
  switch (query.order_by) {
  case ImageSortField::CreatedAt:
    order_sql += "i.created_at " + direction + ", i.id " + direction;
    break;
  case ImageSortField::PublishedAt:
    order_sql += "published_at " + direction + " NULLS LAST, i.id " + direction;
    break;
  case ImageSortField::ScanAt:
    order_sql += "scan_at " + direction + " NULLS LAST, i.id " + direction;
    break;
  case ImageSortField::Similarity:
    order_sql += "sort_score DESC NULLS LAST, i.id DESC";
    break;
  }

  const bool with_score = query.order_by == ImageSortField::Similarity;
  const std::string page_sql =
      std::string(kImageSearchWithSql) + "\nSELECT " + kImageColumnsSql +
      (with_score ? std::string(", ") + kImageSimilarityScoreSql
                  : std::string()) +
      kImageSearchFromSql + order_sql + "\nLIMIT :limit OFFSET :offset";
  const std::string count_sql = std::string(kImageSearchWithSql) +
                                "\nSELECT count(*)" + kImageSearchFromSql;

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());

    std::vector<std::string> checked_ids = include_ids;
    checked_ids.insert(checked_ids.end(), exclude_ids.begin(),
                       exclude_ids.end());
    checked_ids = unique_ids(checked_ids);
    if (!checked_ids.empty()) {
      long long found = 0;
      const std::string checked_array = uuid_array_literal(checked_ids);
      sql << kCountTagsSql, soci::use(checked_array), soci::into(found);
      if (found != static_cast<long long>(checked_ids.size())) {
        return TagimgRepositoryError::UnknownTag;
      }
    }

    ImageSearchResult result;
    if (query.order_by == ImageSortField::Similarity) {
      const std::string embedding_value = pgvector_literal(query.sort_embedding);
      soci::rowset<soci::row> rows =
          (sql.prepare << page_sql, soci::use(entity_value, entity_indicator),
           soci::use(image_value, image_indicator),
           soci::use(artist_value, artist_indicator),
           soci::use(include_value, include_indicator),
           soci::use(exclude_value, exclude_indicator),
           soci::use(include_count), soci::use(expand_include),
           soci::use(expand_exclude), soci::use(include_all),
           soci::use(reference_value, reference_indicator),
           soci::use(embedding_value), soci::use(limit_value),
           soci::use(offset_value));
      for (const soci::row &row : rows) {
        ImageSummary item = parse_image_row(row);
        // 第 8 列是相似度分数；图片没有向量时是 NULL。
        if (row.get_indicator(7) != soci::i_null) {
          item.sort_score = row.get<double>(7);
        }
        result.items.push_back(std::move(item));
      }
    } else {
      soci::rowset<soci::row> rows =
          (sql.prepare << page_sql, soci::use(entity_value, entity_indicator),
           soci::use(image_value, image_indicator),
           soci::use(artist_value, artist_indicator),
           soci::use(include_value, include_indicator),
           soci::use(exclude_value, exclude_indicator),
           soci::use(include_count), soci::use(expand_include),
           soci::use(expand_exclude), soci::use(include_all),
           soci::use(reference_value, reference_indicator),
           soci::use(limit_value), soci::use(offset_value));
      for (const soci::row &row : rows) {
        result.items.push_back(parse_image_row(row));
      }
    }
    if (result.items.size() > query.limit) {
      result.items.pop_back();
      result.has_more = true;
    }
    attach_image_relations(sql, result.items);

    if (query.include_total) {
      long long total = 0;
      sql << count_sql, soci::use(entity_value, entity_indicator),
          soci::use(image_value, image_indicator),
          soci::use(artist_value, artist_indicator),
          soci::use(include_value, include_indicator),
          soci::use(exclude_value, exclude_indicator), soci::use(include_count),
          soci::use(expand_include), soci::use(expand_exclude),
          soci::use(include_all),
          soci::use(reference_value, reference_indicator), soci::into(total);
      result.total = static_cast<std::size_t>(total);
    }

    return result;
  } catch (const std::exception &) {
    return TagimgRepositoryError::QueryFailed;
  }
}

Result<std::size_t, TagimgRepositoryError>
TagimgRepository::update_images(const ImageUpdateInput &input) const {
  if (input.image_ids.empty()) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (input.add_tag_ids.size() + input.remove_tag_ids.size() >
      kMaxArtworkEditTags) {
    return TagimgRepositoryError::InvalidArgument;
  }

  std::set<std::string> image_ids;
  for (const std::string &image_id : input.image_ids) {
    if (!is_uuid(image_id) || !image_ids.insert(image_id).second) {
      return TagimgRepositoryError::InvalidArgument;
    }
  }

  std::set<std::string> add_tag_ids;
  for (const std::string &tag_id : input.add_tag_ids) {
    if (!is_uuid(tag_id) || !add_tag_ids.insert(tag_id).second) {
      return TagimgRepositoryError::InvalidArgument;
    }
  }
  std::set<std::string> remove_tag_ids;
  for (const std::string &tag_id : input.remove_tag_ids) {
    if (!is_uuid(tag_id) || !remove_tag_ids.insert(tag_id).second ||
        add_tag_ids.count(tag_id) > 0) {
      return TagimgRepositoryError::InvalidArgument;
    }
  }
  if (add_tag_ids.empty() && remove_tag_ids.empty()) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());
    soci::transaction transaction(sql);

    const std::string image_array = uuid_array_literal(input.image_ids);
    long long known_images = 0;
    sql << kCountExistingImagesSql, soci::use(image_array),
        soci::into(known_images);
    if (known_images != static_cast<long long>(input.image_ids.size())) {
      return TagimgRepositoryError::UnknownImage;
    }

    std::vector<std::string> checked_tags = input.add_tag_ids;
    checked_tags.insert(checked_tags.end(), input.remove_tag_ids.begin(),
                        input.remove_tag_ids.end());
    const std::string tag_array = uuid_array_literal(checked_tags);
    long long known_tags = 0;
    sql << kCountExistingTagsSql, soci::use(tag_array), soci::into(known_tags);
    if (known_tags != static_cast<long long>(checked_tags.size())) {
      return TagimgRepositoryError::UnknownTag;
    }

    if (!add_tag_ids.empty()) {
      const std::string add_array = uuid_array_literal(input.add_tag_ids);
      // 按 SQL 中占位符出现顺序绑定：先 :tag_ids，再 :image_ids。
      sql << kInsertImageTagsForImagesSql, soci::use(add_array),
          soci::use(image_array);
    }
    if (!remove_tag_ids.empty()) {
      const std::string remove_array =
          uuid_array_literal(input.remove_tag_ids);
      sql << kDeleteImageTagsForImagesSql, soci::use(image_array),
          soci::use(remove_array);
    }

    transaction.commit();
    return input.image_ids.size();
  } catch (const std::exception &) {
    return TagimgRepositoryError::QueryFailed;
  }
}

Result<std::vector<TagMatch>, TagimgRepositoryError>
TagimgRepository::search_tags(const TagSearchQuery &query) const {
  if (query.text.empty() || utf8_length(query.text) > kMaxTagSearchLength) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (query.limit == 0 || query.limit > kMaxTagSearchLimit) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (query.scope == TagSearchScope::Subtree && !is_uuid(query.parent_id)) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  std::string sql_text = "WITH RECURSIVE\n";
  if (query.scope == TagSearchScope::Subtree && !query.direct_only) {
    sql_text += R"sql(subtree(id) AS (
  SELECT id FROM tags WHERE parent_id = CAST(:parent_id AS uuid)
  UNION ALL
  SELECT t.id FROM tags t JOIN subtree s ON t.parent_id = s.id
),
)sql";
  }
  // 标准名和别名都参与匹配；先按 tag 选最佳命中，再去重排序。
  sql_text += R"sql(matched AS (
  SELECT t.id AS tag_id,
         n.name,
         (n.type = 'standard') AS is_standard,
         (lower(n.name) = lower(:text)) AS is_exact,
         (n.name ILIKE :prefix ESCAPE '\') AS is_prefix
  FROM tags t
  JOIN tag_name n ON n.tag_id = t.id AND n.type IN ('standard', 'alias')
  WHERE n.name ILIKE :pattern ESCAPE '\')sql";
  if (query.scope == TagSearchScope::Root) {
    sql_text += "\n    AND t.parent_id IS NULL";
  } else if (query.scope == TagSearchScope::Subtree) {
    if (query.direct_only) {
      sql_text += "\n    AND t.parent_id = CAST(:parent_id AS uuid)";
    } else {
      sql_text += "\n    AND t.id IN (SELECT id FROM subtree)";
    }
  }
  sql_text += R"sql(
),
best AS (
  SELECT DISTINCT ON (tag_id)
         tag_id, name, is_exact, is_prefix
  FROM matched
  ORDER BY tag_id, is_exact DESC, is_prefix DESC, length(name), name,
           is_standard DESC
),
candidates AS (
  SELECT tag_id, name,
         row_number() OVER (
           ORDER BY is_exact DESC, is_prefix DESC, length(name), name, tag_id
         ) AS rn
  FROM best
  ORDER BY is_exact DESC, is_prefix DESC, length(name), name, tag_id
  LIMIT :limit
),
ancestors(tag_id, cur_id, distance) AS (
  SELECT c.tag_id, c.tag_id, 0 FROM candidates c
  UNION ALL
  SELECT a.tag_id, p.parent_id, a.distance + 1
  FROM ancestors a
  JOIN tags p ON p.id = a.cur_id
  WHERE p.parent_id IS NOT NULL
),
segments AS (
  SELECT a.tag_id,
         a.distance,
         COALESCE(n.name, a.cur_id::text) AS segment
  FROM ancestors a
  LEFT JOIN LATERAL (
    SELECT name
    FROM tag_name
    WHERE tag_id = a.cur_id AND type = 'standard'
    ORDER BY id
    LIMIT 1
  ) n ON TRUE
),
aggregated AS (
  SELECT tag_id,
         string_agg(segment, '::' ORDER BY distance DESC) AS path
  FROM segments
  GROUP BY tag_id
)
SELECT c.tag_id::text, c.name, ag.path
FROM candidates c
JOIN aggregated ag ON ag.tag_id = c.tag_id
ORDER BY c.rn)sql";

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());

    std::string text = query.text;
    std::string prefix = like_prefix_pattern(query.text);
    std::string pattern = like_pattern(query.text);
    const int limit_value = static_cast<int>(query.limit);

    std::vector<TagMatch> matches;
    if (query.scope == TagSearchScope::Subtree) {
      std::string parent_id = query.parent_id;
      if (query.direct_only) {
        // 直接子标签模式下 :parent_id 出现在 :pattern 之后，绑定顺序要一致。
        soci::rowset<soci::row> rows =
            (sql.prepare << sql_text, soci::use(text), soci::use(prefix),
             soci::use(pattern), soci::use(parent_id), soci::use(limit_value));
        for (const soci::row &row : rows) {
          matches.push_back(TagMatch{row.get<std::string>(0),
                                     row.get<std::string>(1),
                                     row.get<std::string>(2)});
        }
      } else {
        soci::rowset<soci::row> rows =
            (sql.prepare << sql_text, soci::use(parent_id), soci::use(text),
             soci::use(prefix), soci::use(pattern), soci::use(limit_value));
        for (const soci::row &row : rows) {
          matches.push_back(TagMatch{row.get<std::string>(0),
                                     row.get<std::string>(1),
                                     row.get<std::string>(2)});
        }
      }
    } else {
      soci::rowset<soci::row> rows =
          (sql.prepare << sql_text, soci::use(text), soci::use(prefix),
           soci::use(pattern), soci::use(limit_value));
      for (const soci::row &row : rows) {
        matches.push_back(TagMatch{row.get<std::string>(0),
                                   row.get<std::string>(1),
                                   row.get<std::string>(2)});
      }
    }
    return matches;
  } catch (const std::exception &) {
    return TagimgRepositoryError::QueryFailed;
  }
}

Result<std::vector<ArtworkTagInfo>, TagimgRepositoryError>
TagimgRepository::list_tag_children(
    const std::optional<std::string> &parent_id) const {
  if (parent_id && !is_uuid(*parent_id)) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());

    const std::string parent_value = parent_id.value_or("");
    soci::indicator parent_indicator =
        parent_id ? soci::i_ok : soci::i_null;

    std::vector<std::string> ids;
    soci::rowset<soci::row> rows =
        (sql.prepare << kTagChildrenSql,
         soci::use(parent_value, parent_indicator));
    for (const soci::row &row : rows) {
      ids.push_back(row.get<std::string>(0));
    }

    auto infos = load_tag_infos(sql, ids);
    std::vector<ArtworkTagInfo> result;
    result.reserve(ids.size());
    for (const std::string &id : ids) {
      auto found = infos.find(id);
      if (found != infos.end()) {
        result.push_back(std::move(found->second));
      }
    }
    return result;
  } catch (const std::exception &) {
    return TagimgRepositoryError::QueryFailed;
  }
}

Result<std::vector<TagTreeMatchInfo>, TagimgRepositoryError>
TagimgRepository::search_tag_tree(const std::string &text) const {
  const std::string trimmed = trim_ascii(text);
  if (trimmed.empty() || utf8_length(trimmed) > kMaxTagSearchLength) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());

    const std::string prefix = like_prefix_pattern(trimmed);
    const std::string pattern = like_pattern(trimmed);

    struct MatchRow {
      std::string id;
      std::string name;
      bool alias = false;
    };
    std::vector<MatchRow> matches;
    std::vector<std::string> match_ids;
    soci::rowset<soci::row> rows =
        (sql.prepare << kTagTreeMatchSql, soci::use(trimmed),
         soci::use(prefix), soci::use(pattern));
    for (const soci::row &row : rows) {
      MatchRow item;
      item.id = row.get<std::string>(0);
      item.name = row.get<std::string>(1);
      item.alias = row.get<int>(2) == 0;
      match_ids.push_back(item.id);
      matches.push_back(std::move(item));
    }
    if (matches.empty()) {
      return std::vector<TagTreeMatchInfo>{};
    }

    // 收集命中节点自身和祖先 ID，随后一次批量读取节点详情。
    std::map<std::string, std::vector<std::string>> ancestor_ids;
    std::vector<std::string> all_ids = match_ids;
    const std::string match_array = uuid_array_literal(match_ids);
    soci::rowset<soci::row> ancestors =
        (sql.prepare << kTagTreeAncestorsSql, soci::use(match_array));
    for (const soci::row &row : ancestors) {
      const std::string match_id = row.get<std::string>(0);
      const std::string ancestor_id = row.get<std::string>(1);
      const int distance = row.get<int>(2);
      if (distance > 0) {
        ancestor_ids[match_id].push_back(ancestor_id);
        all_ids.push_back(ancestor_id);
      }
    }

    auto infos = load_tag_infos(sql, all_ids);
    std::vector<TagTreeMatchInfo> result;
    result.reserve(matches.size());
    for (const MatchRow &match : matches) {
      auto node = infos.find(match.id);
      if (node == infos.end()) {
        continue;
      }
      TagTreeMatchInfo item;
      item.node = node->second;
      item.matched_name = match.name;
      item.matched_alias = match.alias;
      for (const std::string &ancestor_id : ancestor_ids[match.id]) {
        auto ancestor = infos.find(ancestor_id);
        if (ancestor != infos.end()) {
          item.ancestors.push_back(ancestor->second);
        }
      }
      result.push_back(std::move(item));
    }
    return result;
  } catch (const std::exception &) {
    return TagimgRepositoryError::QueryFailed;
  }
}

Result<ArtworkTagInfo, TagimgRepositoryError>
TagimgRepository::create_tag(const TagCreateInput &input) const {
  const std::string name = trim_ascii(input.name);
  if (!valid_tag_name(name)) {
    return TagimgRepositoryError::InvalidArgument;
  }
  auto normalized_aliases = normalize_tag_aliases(input.aliases, name);
  if (normalized_aliases.has_error()) {
    return normalized_aliases.error();
  }
  auto aliases = std::move(normalized_aliases).value();

  if (input.parent_id && !is_uuid(*input.parent_id)) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());
    soci::transaction transaction(sql);

    if (input.parent_id &&
        count_rows(sql, kCountTagSql, *input.parent_id) == 0) {
      return TagimgRepositoryError::TagParentNotFound;
    }

    acquire_tag_lock(sql, tag_lock_key(input.parent_id));

    const std::string parent_value = input.parent_id.value_or("");
    soci::indicator parent_indicator =
        input.parent_id ? soci::i_ok : soci::i_null;
    long long conflicts = 0;
    sql << kCountCreateNameConflictSql,
        soci::use(parent_value, parent_indicator), soci::use(name),
        soci::into(conflicts);
    if (conflicts > 0) {
      return TagimgRepositoryError::TagNameConflict;
    }

    std::string tag_id;
    {
      soci::rowset<soci::row> rows =
          (sql.prepare << kInsertTagSql,
           soci::use(parent_value, parent_indicator));
      const auto row = rows.begin();
      if (row != rows.end()) {
        tag_id = row->get<std::string>(0);
      }
    }
    if (tag_id.empty()) {
      return TagimgRepositoryError::QueryFailed;
    }

    sql << kInsertTagNameSql, soci::use(tag_id), soci::use(name);
    for (const std::string &alias : aliases) {
      sql << kInsertAliasNameSql, soci::use(tag_id), soci::use(alias);
    }

    auto info = load_tag_info(sql, tag_id);
    if (!info) {
      return TagimgRepositoryError::QueryFailed;
    }
    transaction.commit();
    return std::move(*info);
  } catch (const std::exception &) {
    return TagimgRepositoryError::QueryFailed;
  }
}

Result<ArtworkTagInfo, TagimgRepositoryError>
TagimgRepository::update_tag(const std::string &tag_id,
                             const TagUpdateInput &input) const {
  if (!is_uuid(tag_id)) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (input.clear_parent && input.parent_id) {
    return TagimgRepositoryError::InvalidArgument;
  }

  std::optional<std::string> requested_name;
  if (input.name) {
    requested_name = trim_ascii(*input.name);
    if (!valid_tag_name(*requested_name)) {
      return TagimgRepositoryError::InvalidArgument;
    }
  }
  if (input.parent_id && !is_uuid(*input.parent_id)) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (!requested_name && !input.aliases && !input.parent_id &&
      !input.clear_parent) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());
    soci::transaction transaction(sql);

    std::optional<std::string> current_parent;
    {
      soci::rowset<soci::row> rows =
          (sql.prepare << kSelectTagForUpdateSql, soci::use(tag_id));
      const auto row = rows.begin();
      if (row == rows.end()) {
        return TagimgRepositoryError::UnknownTag;
      }
      std::string parent_value = row->get<std::string>(0);
      if (!parent_value.empty()) {
        current_parent = std::move(parent_value);
      }
    }

    const auto current_standard = find_standard_name(sql, tag_id);
    const std::string final_name =
        requested_name
            ? *requested_name
            : (current_standard ? current_standard->second : std::string{});

    std::optional<std::string> final_parent = current_parent;
    if (input.clear_parent) {
      final_parent.reset();
    } else if (input.parent_id) {
      final_parent = *input.parent_id;
    }

    if (final_parent && final_parent != current_parent) {
      if (count_rows(sql, kCountTagSql, *final_parent) == 0) {
        return TagimgRepositoryError::TagParentNotFound;
      }
      long long cycle = 0;
      sql << kCheckTagCycleSql, soci::use(*final_parent), soci::use(tag_id),
          soci::into(cycle);
      if (cycle > 0) {
        return TagimgRepositoryError::TagCycle;
      }
    }

    std::vector<std::string> lock_keys;
    lock_keys.push_back(tag_lock_key(current_parent));
    if (final_parent != current_parent) {
      lock_keys.push_back(tag_lock_key(final_parent));
    }
    std::sort(lock_keys.begin(), lock_keys.end());
    lock_keys.erase(std::unique(lock_keys.begin(), lock_keys.end()),
                    lock_keys.end());
    for (const std::string &key : lock_keys) {
      acquire_tag_lock(sql, key);
    }

    std::optional<std::vector<std::string>> final_aliases;
    if (input.aliases) {
      auto normalized_aliases =
          normalize_tag_aliases(*input.aliases, final_name);
      if (normalized_aliases.has_error()) {
        return normalized_aliases.error();
      }
      final_aliases = std::move(normalized_aliases).value();
    } else if (requested_name && !final_name.empty()) {
      long long alias_conflict = 0;
      sql << kCountTagAliasNameSql, soci::use(tag_id), soci::use(final_name),
          soci::into(alias_conflict);
      if (alias_conflict > 0) {
        return TagimgRepositoryError::TagNameConflict;
      }
    }

    if (!final_name.empty()) {
      const std::string parent_value = final_parent.value_or("");
      soci::indicator parent_indicator =
          final_parent ? soci::i_ok : soci::i_null;
      long long conflicts = 0;
      sql << kCountUpdateNameConflictSql,
          soci::use(parent_value, parent_indicator), soci::use(tag_id),
          soci::use(final_name), soci::into(conflicts);
      if (conflicts > 0) {
        return TagimgRepositoryError::TagNameConflict;
      }
    }

    // 先删旧别名，避免“标准名改成旧别名”时触发 (tag_id, name) 唯一约束；
    // 再改标准名，最后插新别名，避免“新别名复用旧标准名”时冲突。
    if (final_aliases) {
      sql << kDeleteTagAliasesSql, soci::use(tag_id);
    }

    if (requested_name) {
      if (current_standard) {
        sql << kUpdateTagNameSql, soci::use(final_name),
            soci::use(current_standard->first);
      } else {
        sql << kInsertTagNameSql, soci::use(tag_id), soci::use(final_name);
      }
    }

    if (final_aliases) {
      for (const std::string &alias : *final_aliases) {
        sql << kInsertAliasNameSql, soci::use(tag_id), soci::use(alias);
      }
    }

    if (final_parent != current_parent) {
      const std::string parent_value = final_parent.value_or("");
      soci::indicator parent_indicator =
          final_parent ? soci::i_ok : soci::i_null;
      sql << kUpdateTagParentSql, soci::use(parent_value, parent_indicator),
          soci::use(tag_id);
    }

    auto info = load_tag_info(sql, tag_id);
    if (!info) {
      return TagimgRepositoryError::QueryFailed;
    }
    transaction.commit();
    return std::move(*info);
  } catch (const std::exception &) {
    return TagimgRepositoryError::QueryFailed;
  }
}

Result<DeleteArtworkResult, TagimgRepositoryError>
TagimgRepository::delete_artworks(
    const std::vector<std::string> &artwork_ids) const {
  if (artwork_ids.empty()) {
    return TagimgRepositoryError::InvalidArgument;
  }

  // 先校验全部 ID，非法 UUID 不做任何删除。
  std::vector<std::string> ids;
  ids.reserve(artwork_ids.size());
  std::set<std::string> seen;
  for (const std::string &artwork_id : artwork_ids) {
    if (!is_uuid(artwork_id)) {
      return TagimgRepositoryError::InvalidArgument;
    }
    std::string key = ascii_lower(artwork_id);
    if (seen.insert(key).second) {
      ids.push_back(artwork_id);
    }
  }

  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());
    soci::transaction transaction(sql);

    const std::string artwork_array = uuid_array_literal(ids);

    // 先记下这些作品的图片并集，删除后再判断哪些失去了全部引用。
    std::vector<std::string> image_ids;
    {
      soci::rowset<soci::row> image_rows = (sql.prepare
          << kArtworkImageIdsBatchSql, soci::use(artwork_array));
      for (const soci::row &row : image_rows) {
        image_ids.push_back(row.get<std::string>(0));
      }
    }

    // rowset 用完即销毁，避免提交事务时还留着未关闭的游标。
    std::size_t deleted_count = 0;
    {
      soci::rowset<soci::row> deleted_rows = (sql.prepare
          << kDeleteArtworksSql, soci::use(artwork_array));
      for (const soci::row &row : deleted_rows) {
        (void)row;
        ++deleted_count;
      }
    }

    DeleteArtworkResult result;
    result.deleted = deleted_count > 0;
    if (result.deleted && !image_ids.empty()) {
      const std::string image_array = uuid_array_literal(image_ids);
      long long orphaned = 0;
      sql << kCountOrphanAmongSql, soci::use(image_array), soci::into(orphaned);
      result.newly_orphaned_image_count = static_cast<std::size_t>(orphaned);
    }

    transaction.commit();
    return result;
  } catch (const std::exception &) {
    return TagimgRepositoryError::QueryFailed;
  }
}

Result<DeleteImageResult, TagimgRepositoryError>
TagimgRepository::delete_images(const std::vector<std::string> &image_ids) const {
  if (image_ids.empty()) {
    return TagimgRepositoryError::InvalidArgument;
  }

  // 先校验全部 ID，非法 UUID 不做任何删除。
  std::vector<std::string> ids;
  ids.reserve(image_ids.size());
  std::set<std::string> seen;
  for (const std::string &image_id : image_ids) {
    if (!is_uuid(image_id)) {
      return TagimgRepositoryError::InvalidArgument;
    }
    std::string key = ascii_lower(image_id);
    if (seen.insert(key).second) {
      ids.push_back(image_id);
    }
  }

  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());
    soci::transaction transaction(sql);

    const std::string image_array = uuid_array_literal(ids);

    // 图片行删除后 artwork_images 会级联消失，先根据当前关联刷新作品。
    sql << kTouchArtworksForImagesSql, soci::use(image_array);

    std::vector<std::string> deleted_ids;
    {
      soci::rowset<soci::row> deleted_rows = (sql.prepare
          << kDeleteImagesSql, soci::use(image_array));
      for (const soci::row &row : deleted_rows) {
        deleted_ids.push_back(row.get<std::string>(0));
      }
    }

    DeleteImageResult result;
    result.deleted_count = deleted_ids.size();
    std::set<std::string> deleted_keys;
    for (const std::string &deleted_id : deleted_ids) {
      deleted_keys.insert(ascii_lower(deleted_id));
    }
    for (const std::string &image_id : ids) {
      if (deleted_keys.find(ascii_lower(image_id)) == deleted_keys.end()) {
        result.missing_ids.push_back(image_id);
      }
    }

    transaction.commit();
    return result;
  } catch (const std::exception &) {
    return TagimgRepositoryError::QueryFailed;
  }
}

struct ArtworkUpdateTransaction::Impl {
  Impl(soci::connection_pool &pool, std::string id)
      : lease(pool), sql(pool.at(lease.position())), txn(sql),
        artwork_id(std::move(id)) {}

  db_detail::PoolLease lease;
  soci::session &sql;
  soci::transaction txn;
  std::string artwork_id;
  bool dirty = false;
  bool published_at_set = false;
  bool published_at_cleared = false;
  // 本次事务里已经加过 / 删过的标签，用来拦住自相矛盾的调用。
  std::set<std::string> added_tag_ids;
  std::set<std::string> removed_tag_ids;
};

ArtworkUpdateTransaction::ArtworkUpdateTransaction(
    std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {
}

ArtworkUpdateTransaction::~ArtworkUpdateTransaction() = default;

ArtworkUpdateTransaction::ArtworkUpdateTransaction(
    ArtworkUpdateTransaction &&) noexcept = default;

Result<void, TagimgRepositoryError>
ArtworkUpdateTransaction::set_title(std::string title) {
  if (validate_title(title).has_value()) {
    return TagimgRepositoryError::InvalidArgument;
  }
  impl_->sql << kUpdateArtworkTitleSql, soci::use(title),
      soci::use(impl_->artwork_id);
  impl_->dirty = true;
  return {};
}

Result<void, TagimgRepositoryError>
ArtworkUpdateTransaction::set_published_at(std::string published_at) {
  if (impl_->published_at_cleared) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (validate_published_at(published_at).has_value()) {
    return TagimgRepositoryError::InvalidArgument;
  }
  impl_->sql << kUpdateArtworkPublishedAtSql, soci::use(published_at),
      soci::use(impl_->artwork_id);
  impl_->published_at_set = true;
  impl_->dirty = true;
  return {};
}

Result<void, TagimgRepositoryError>
ArtworkUpdateTransaction::clear_published_at() {
  if (impl_->published_at_set) {
    return TagimgRepositoryError::InvalidArgument;
  }
  impl_->sql << kClearArtworkPublishedAtSql, soci::use(impl_->artwork_id);
  impl_->published_at_cleared = true;
  impl_->dirty = true;
  return {};
}

Result<void, TagimgRepositoryError>
ArtworkUpdateTransaction::add_tag_ids(
    const std::vector<std::string> &tag_ids) {
  if (tag_ids.empty()) {
    return {};
  }

  // 同一批里不能重复，也不能是本次事务已经摘掉的标签。
  std::set<std::string> incoming;
  for (const std::string &tag_id : tag_ids) {
    if (!is_uuid(tag_id) || impl_->removed_tag_ids.count(tag_id) > 0 ||
        !incoming.insert(tag_id).second) {
      return TagimgRepositoryError::InvalidArgument;
    }
  }
  std::set<std::string> merged = impl_->added_tag_ids;
  merged.insert(incoming.begin(), incoming.end());
  if (merged.size() + impl_->removed_tag_ids.size() >
      kMaxArtworkEditTags) {
    return TagimgRepositoryError::InvalidArgument;
  }

  const std::string tag_array = uuid_array_literal(tag_ids);
  long long known = 0;
  impl_->sql << kCountExistingTagsSql, soci::use(tag_array),
      soci::into(known);
  if (known != static_cast<long long>(tag_ids.size())) {
    return TagimgRepositoryError::UnknownTag;
  }

  impl_->sql << kInsertImageTagsForArtworkSql, soci::use(tag_array),
      soci::use(impl_->artwork_id);
  impl_->added_tag_ids.insert(incoming.begin(), incoming.end());
  impl_->dirty = true;
  return {};
}

Result<void, TagimgRepositoryError>
ArtworkUpdateTransaction::remove_tag_ids(
    const std::vector<std::string> &tag_ids) {
  if (tag_ids.empty()) {
    return {};
  }

  std::set<std::string> incoming;
  for (const std::string &tag_id : tag_ids) {
    if (!is_uuid(tag_id) || impl_->added_tag_ids.count(tag_id) > 0 ||
        !incoming.insert(tag_id).second) {
      return TagimgRepositoryError::InvalidArgument;
    }
  }
  std::set<std::string> merged = impl_->removed_tag_ids;
  merged.insert(incoming.begin(), incoming.end());
  if (merged.size() + impl_->added_tag_ids.size() >
      kMaxArtworkEditTags) {
    return TagimgRepositoryError::InvalidArgument;
  }

  const std::string tag_array = uuid_array_literal(tag_ids);
  long long known = 0;
  impl_->sql << kCountExistingTagsSql, soci::use(tag_array),
      soci::into(known);
  if (known != static_cast<long long>(tag_ids.size())) {
    return TagimgRepositoryError::UnknownTag;
  }

  impl_->sql << kDeleteImageTagsSql, soci::use(impl_->artwork_id),
      soci::use(tag_array);
  impl_->removed_tag_ids.insert(incoming.begin(), incoming.end());
  impl_->dirty = true;
  return {};
}

Result<void, TagimgRepositoryError>
ArtworkUpdateTransaction::reorder_images(
    const std::vector<std::string> &image_ids) {
  if (validate_artwork_image_order(image_ids).has_value()) {
    return TagimgRepositoryError::InvalidArgument;
  }
  for (const std::string &image_id : image_ids) {
    if (!is_uuid(image_id)) {
      return TagimgRepositoryError::InvalidArgument;
    }
  }

  // 必须是当前图片集合的一个完整排列，不能借排序增删图片。
  std::set<std::string> current_image_ids;
  {
    soci::rowset<soci::row> rows =
        (impl_->sql.prepare << kLockArtworkImagesSql,
         soci::use(impl_->artwork_id));
    for (const soci::row &row : rows) {
      current_image_ids.insert(row.get<std::string>(0));
    }
  }
  if (current_image_ids.size() != image_ids.size()) {
    return TagimgRepositoryError::InvalidArgument;
  }
  for (const std::string &image_id : image_ids) {
    if (current_image_ids.count(image_id) == 0) {
      return TagimgRepositoryError::InvalidArgument;
    }
  }

  const std::string image_array = uuid_array_literal(image_ids);
  impl_->sql << kUpdateArtworkImagePositionsSql, soci::use(image_array),
      soci::use(impl_->artwork_id);
  impl_->dirty = true;
  return {};
}

Result<std::size_t, TagimgRepositoryError>
ArtworkUpdateTransaction::add_scanned_tags(
    const std::vector<ImageScannedTags> &tags) {
  if (tags.empty()) {
    return std::size_t{0};
  }

  // 只允许写当前作品的图片，避免拿错 image_id 串到别的作品。
  std::set<std::string> current_images;
  {
    soci::rowset<soci::row> rows =
        (impl_->sql.prepare << kLockArtworkImagesSql,
         soci::use(impl_->artwork_id));
    for (const soci::row &row : rows) {
      current_images.insert(row.get<std::string>(0));
    }
  }

  for (const ImageScannedTags &item : tags) {
    if (!is_uuid(item.image_id) ||
        current_images.count(item.image_id) == 0) {
      return TagimgRepositoryError::InvalidArgument;
    }
    for (const ArtworkTag &tag : item.tags) {
      if (tag.name.empty() || tag.category.empty()) {
        return TagimgRepositoryError::InvalidArgument;
      }
    }
  }

  const auto root_id = find_root_tag(impl_->sql);
  if (!root_id) {
    return TagimgRepositoryError::TagHierarchyMissing;
  }

  // 分类标签必须已经存在，与 write_artwork 的规则保持一致。
  std::map<std::string, std::string> categories;
  for (const ImageScannedTags &item : tags) {
    for (const ArtworkTag &tag : item.tags) {
      if (categories.count(tag.category) > 0) {
        continue;
      }
      const auto category_id =
          find_child_tag(impl_->sql, *root_id, tag.category);
      if (!category_id) {
        return TagimgRepositoryError::TagHierarchyMissing;
      }
      categories.emplace(tag.category, *category_id);
    }
  }

  std::set<std::string> seen;
  for (const ImageScannedTags &item : tags) {
    for (const ArtworkTag &tag : item.tags) {
      const std::string &category_id = categories.at(tag.category);
      std::string name = tag.name;

      auto tag_id = find_child_tag(impl_->sql, category_id, name);
      if (!tag_id) {
        std::string created =
            insert_returning_id(impl_->sql, kInsertTagSql, category_id);
        if (created.empty()) {
          return TagimgRepositoryError::QueryFailed;
        }
        impl_->sql << kInsertTagNameSql, soci::use(created), soci::use(name);
        tag_id = std::move(created);
      }

      seen.insert(*tag_id);
      impl_->sql << kInsertImageTagSql, soci::use(item.image_id),
          soci::use(*tag_id);
    }
  }

  impl_->dirty = true;
  return seen.size();
}

Result<std::size_t, TagimgRepositoryError>
ArtworkUpdateTransaction::replace_embeddings(
    const std::vector<ImageEmbeddingInput> &items) {
  for (const ImageEmbeddingInput &item : items) {
    if (item.image_id.empty() || !is_uuid(item.image_id) ||
        item.values.size() != kClipEmbeddingDim) {
      return TagimgRepositoryError::InvalidArgument;
    }
  }
  if (items.empty()) {
    return std::size_t{0};
  }

  for (const ImageEmbeddingInput &item : items) {
    const std::string literal = pgvector_literal(item.values);
    impl_->sql << kUpsertClipEmbeddingSql, soci::use(item.image_id),
        soci::use(literal);
  }
  impl_->dirty = true;
  return items.size();
}

Result<std::size_t, TagimgRepositoryError>
ArtworkUpdateTransaction::touch_image_scans(
    const std::vector<std::string> &image_ids) {
  for (const std::string &image_id : image_ids) {
    if (!is_uuid(image_id)) {
      return TagimgRepositoryError::InvalidArgument;
    }
  }
  if (image_ids.empty()) {
    return std::size_t{0};
  }

  const std::string image_array = uuid_array_literal(image_ids);
  impl_->sql << kTouchImageScansSql, soci::use(image_array);
  impl_->dirty = true;
  return image_ids.size();
}

Result<std::size_t, TagimgRepositoryError>
ArtworkUpdateTransaction::update_image_metadata(
    const std::vector<ImageMetadataInput> &items) {
  if (items.empty()) {
    return std::size_t{0};
  }

  // 只允许写当前作品的图片，避免拿错 image_id 串到别的作品。
  std::set<std::string> current_images;
  {
    soci::rowset<soci::row> rows =
        (impl_->sql.prepare << kLockArtworkImagesSql,
         soci::use(impl_->artwork_id));
    for (const soci::row &row : rows) {
      current_images.insert(row.get<std::string>(0));
    }
  }

  std::set<std::string> seen;
  for (const ImageMetadataInput &item : items) {
    if (!is_uuid(item.image_id) ||
        current_images.count(item.image_id) == 0 ||
        !seen.insert(item.image_id).second ||
        !is_valid_image_metadata(item.metadata)) {
      return TagimgRepositoryError::InvalidArgument;
    }
  }

  for (const ImageMetadataInput &item : items) {
    update_image_metadata_row(impl_->sql, item.image_id, item.metadata);
  }
  impl_->dirty = true;
  return items.size();
}

Result<void, TagimgRepositoryError> ArtworkUpdateTransaction::commit() {
  if (!impl_->dirty) {
    return TagimgRepositoryError::InvalidArgument;
  }
  impl_->sql << kTouchArtworkSql, soci::use(impl_->artwork_id);
  impl_->txn.commit();
  return {};
}

Result<std::optional<ArtworkUpdateTransaction>, TagimgRepositoryError>
TagimgRepository::begin_artwork_update(const std::string &artwork_id) const {
  if (!is_uuid(artwork_id)) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  try {
    auto state = std::make_unique<ArtworkUpdateTransaction::Impl>(
        *impl_->pool, artwork_id);

    // 先锁行：锁不到说明作品已经不存在。
    bool found = false;
    {
      soci::rowset<soci::row> rows =
          (state->sql.prepare << kLockArtworkSql,
           soci::use(state->artwork_id));
      for (const soci::row &row : rows) {
        (void)row;
        found = true;
      }
    }
    if (!found) {
      return std::optional<ArtworkUpdateTransaction>{};
    }

    return std::optional<ArtworkUpdateTransaction>{
        ArtworkUpdateTransaction(std::move(state))};
  } catch (const std::exception &) {
    return TagimgRepositoryError::QueryFailed;
  }
}

Result<OrphanImageResult, TagimgRepositoryError>
TagimgRepository::list_orphan_images(const OrphanImageQuery &query) const {
  if (query.limit == 0 || query.limit > 100 || query.offset > 10000) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());

    // 多取一条用来判断是否还有下一页。
    const int fetch_limit = static_cast<int>(query.limit) + 1;
    const int offset_value = static_cast<int>(query.offset);

    OrphanImageResult result;
    soci::rowset<soci::row> rows =
        (sql.prepare << kOrphanImagesSql, soci::use(fetch_limit),
         soci::use(offset_value));
    for (const soci::row &row : rows) {
      OrphanImage image;
      image.id = row.get<std::string>(0);
      if (row.get_indicator(1) != soci::i_null) {
        image.width = row.get<int>(1);
      }
      if (row.get_indicator(2) != soci::i_null) {
        image.height = row.get<int>(2);
      }
      if (row.get_indicator(3) != soci::i_null) {
        image.frame_count = row.get<int>(3);
      }
      image.created_at = row.get<std::string>(4);
      if (row.get_indicator(5) != soci::i_null) {
        image.scan_at = row.get<std::string>(5);
      }
      result.items.push_back(std::move(image));
    }

    if (result.items.size() > query.limit) {
      result.has_more = true;
      result.items.resize(query.limit);
    }

    if (query.include_total) {
      long long total = 0;
      sql << kCountOrphanImagesSql, soci::into(total);
      result.total = static_cast<std::size_t>(total);
    }

    return result;
  } catch (const std::exception &) {
    return TagimgRepositoryError::QueryFailed;
  }
}

Result<std::size_t, TagimgRepositoryError>
TagimgRepository::save_clip_embeddings(
    const std::vector<ImageEmbeddingInput> &items) const {
  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }
  for (const ImageEmbeddingInput &item : items) {
    if (item.image_id.empty() ||
        item.values.size() != kClipEmbeddingDim) {
      return TagimgRepositoryError::InvalidArgument;
    }
  }
  if (items.empty()) {
    return std::size_t{0};
  }

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());
    soci::transaction transaction(sql);
    for (const ImageEmbeddingInput &item : items) {
      const std::string literal = pgvector_literal(item.values);
      sql << kUpsertClipEmbeddingSql, soci::use(item.image_id),
          soci::use(literal);
    }
    transaction.commit();
    return items.size();
  } catch (const std::exception &) {
    return TagimgRepositoryError::QueryFailed;
  }
}

Result<std::optional<std::vector<float>>, TagimgRepositoryError>
TagimgRepository::find_clip_embedding(const std::string &image_id) const {
  if (image_id.empty()) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());
    soci::rowset<soci::row> rows =
        (sql.prepare << kFindClipEmbeddingSql, soci::use(image_id));
    std::optional<std::vector<float>> result;
    for (const soci::row &row : rows) {
      if (row.get_indicator(0) == soci::i_null) {
        continue;
      }
      // pgvector 的文本形式是 [a,b,c]，这里直接按逗号切分。
      const std::string text = row.get<std::string>(0);
      std::vector<float> values;
      values.reserve(kClipEmbeddingDim);
      const char *cursor = text.c_str();
      if (*cursor == '[') {
        ++cursor;
      }
      while (*cursor != '\0' && *cursor != ']') {
        char *end = nullptr;
        const double value = std::strtod(cursor, &end);
        if (end == cursor) {
          return TagimgRepositoryError::QueryFailed;
        }
        values.push_back(static_cast<float>(value));
        cursor = end;
        if (*cursor == ',') {
          ++cursor;
        }
      }
      result = std::move(values);
      break;
    }
    return result;
  } catch (const std::exception &) {
    return TagimgRepositoryError::QueryFailed;
  }
}

Result<std::vector<ImageEmbeddingRow>, TagimgRepositoryError>
TagimgRepository::list_images_missing_embedding(std::size_t limit,
                                                std::size_t offset) const {
  if (limit == 0) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());
    const int limit_value = static_cast<int>(limit);
    const int offset_value = static_cast<int>(offset);
    soci::rowset<soci::row> rows =
        (sql.prepare << kListMissingClipEmbeddingSql,
         soci::use(limit_value), soci::use(offset_value));
    std::vector<ImageEmbeddingRow> result;
    for (const soci::row &row : rows) {
      ImageEmbeddingRow item;
      item.image_id = row.get<std::string>(0);
      if (row.get_indicator(1) != soci::i_null) {
        item.entity_id = row.get<std::string>(1);
      }
      result.push_back(std::move(item));
    }
    return result;
  } catch (const std::exception &) {
    return TagimgRepositoryError::QueryFailed;
  }
}

} // namespace tagimg
