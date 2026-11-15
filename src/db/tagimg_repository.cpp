#include "db/tagimg_repository.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
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

// 重新扫描已有图片时只更新时间，宽高仍由后续的解码流程回填。
constexpr const char *kTouchImageScanSql = R"sql(
UPDATE images
SET scan_at = now()
WHERE id = :image_id
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

// 删除前先记下这个作品的图片，删除后再统计其中有哪些失去了全部引用。
constexpr const char *kArtworkImageIdsSql = R"sql(
SELECT image_id::text
FROM artwork_images
WHERE artwork_id = :artwork_id
)sql";

constexpr const char *kDeleteArtworkSql = R"sql(
DELETE FROM artworks
WHERE id = :artwork_id
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

constexpr const char *kInsertArtworkTagsSql = R"sql(
INSERT INTO artwork_tags (artwork_id, tag_id)
SELECT :artwork_id, t.id
FROM tags t
WHERE t.id = ANY(CAST(:tag_ids AS uuid[]))
ON CONFLICT DO NOTHING
)sql";

constexpr const char *kDeleteArtworkTagsSql = R"sql(
DELETE FROM artwork_tags
WHERE artwork_id = :artwork_id
  AND tag_id = ANY(CAST(:tag_ids AS uuid[]))
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
INSERT INTO images (entity_id, scan_at)
VALUES (:entity_id, now())
RETURNING id::text
)sql";

constexpr const char *kInsertArtworkImageSql = R"sql(
INSERT INTO artwork_images (artwork_id, image_id, position)
VALUES (:artwork_id, :image_id, :position)
)sql";

constexpr const char *kInsertArtworkTagSql = R"sql(
INSERT INTO artwork_tags (artwork_id, tag_id)
VALUES (:artwork_id, :tag_id)
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

// LIKE 模式里转义 % _ \，保证按字面匹配。
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

// 实体模式与图片模式共用的写库参数。
struct ArtworkWriteInput {
  std::string title;
  std::optional<std::string> description;
  std::optional<std::string> published_at;
  std::vector<std::string> tag_ids;
  std::vector<std::string> artist_ids;
  std::vector<ArtworkTag> scanned_tags;
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

  try {
    soci::transaction transaction(sql);

    const auto root_id = find_root_tag(sql);
    if (!root_id) {
      return TagimgRepositoryError::TagHierarchyMissing;
    }

    // 先把用到的分类标签全部解析出来；缺任何一个都不写库。
    std::map<std::string, std::string> categories;
    for (const ArtworkTag &tag : input.scanned_tags) {
      if (categories.find(tag.category) != categories.end()) {
        continue;
      }
      const auto category_id = find_child_tag(sql, *root_id, tag.category);
      if (!category_id) {
        return TagimgRepositoryError::TagHierarchyMissing;
      }
      categories.emplace(tag.category, *category_id);
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
            insert_returning_id(sql, kInsertImageSql, entity_id);
        if (image_id.empty()) {
          return TagimgRepositoryError::QueryFailed;
        }

        const int position = static_cast<int>(index);
        sql << kInsertArtworkImageSql, soci::use(saved.artwork_id),
            soci::use(image_id), soci::use(position);
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
          sql << kTouchImageScanSql, soci::use(image_id);
        }
        saved.image_ids.push_back(image_id);
      }
    }

    // 调用方直接指定的标签：必须已经存在。
    for (const std::string &id : input.tag_ids) {
      std::string tag_id = id;
      if (count_rows(sql, kCountTagSql, tag_id) == 0) {
        return TagimgRepositoryError::UnknownTag;
      }
      sql << kInsertArtworkTagSql, soci::use(saved.artwork_id),
          soci::use(tag_id);
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
    for (const ArtworkTag &tag : input.scanned_tags) {
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

      sql << kInsertArtworkTagSql, soci::use(saved.artwork_id),
          soci::use(*tag_id);
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
        FROM artwork_tags at
        JOIN include_scope s ON s.tag_id = at.tag_id
        WHERE at.artwork_id = a.id)
       >= CASE WHEN p.include_all THEN p.include_count ELSE 1 END)
  AND (p.exclude_tags IS NULL OR NOT EXISTS (
        SELECT 1
        FROM artwork_tags at
        JOIN exclude_scope s ON s.tag_id = at.tag_id
        WHERE at.artwork_id = a.id))
)sql";

constexpr const char *kArtworkImagesSql = R"sql(
SELECT ai.artwork_id::text, ai.image_id::text
FROM artwork_images ai
WHERE ai.artwork_id = ANY(CAST(:ids AS uuid[]))
ORDER BY ai.artwork_id, ai.position
)sql";

constexpr const char *kArtworkTagsSql = R"sql(
SELECT at.artwork_id::text, at.tag_id::text
FROM artwork_tags at
WHERE at.artwork_id = ANY(CAST(:ids AS uuid[]))
ORDER BY at.artwork_id, at.tag_id
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
  LEFT JOIN tag_name n ON n.tag_id = a.cur_id AND n.type = 'standard'
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
       a.path
FROM aggregated a
LEFT JOIN tag_name n ON n.tag_id = a.tag_id AND n.type = 'standard'
)sql";

constexpr const char *kArtworkAliasesSql = R"sql(
SELECT tn.tag_id::text, tn.name
FROM tag_name tn
WHERE tn.type = 'alias' AND tn.tag_id = ANY(CAST(:tag_ids AS uuid[]))
ORDER BY tn.tag_id, tn.name
)sql";

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
  case TagimgRepositoryError::TagHierarchyMissing:
    return "TagHierarchyMissing";
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
  for (const ArtworkTag &tag : input.scanned_tags) {
    if (tag.name.empty() || tag.category.empty()) {
      return TagimgRepositoryError::InvalidArgument;
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
  for (const ArtworkTag &tag : input.scanned_tags) {
    if (tag.name.empty() || tag.category.empty()) {
      return TagimgRepositoryError::InvalidArgument;
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
  }

  const std::string page_sql =
      std::string(kArtworkSearchWithSql) + "\nSELECT " + kArtworkColumnsSql +
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
    soci::rowset<soci::row> rows =
        (sql.prepare << page_sql, soci::use(title_value, title_indicator),
         soci::use(entity_value, entity_indicator),
         soci::use(artist_value, artist_indicator),
         soci::use(include_value, include_indicator),
         soci::use(exclude_value, exclude_indicator), soci::use(include_count),
         soci::use(expand_include), soci::use(expand_exclude),
         soci::use(include_all), soci::use(limit_value),
         soci::use(offset_value));
    for (const soci::row &row : rows) {
      result.items.push_back(parse_artwork_row(row));
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
  // 先算出候选和名次再拼路径：名次在窗口函数里计算，
  // 同一层排序键只出现一次，便于按位置绑定参数。
  sql_text += R"sql(candidates AS (
  SELECT id, name,
         row_number() OVER (
           ORDER BY is_exact DESC, is_prefix DESC, length(name), name, id
         ) AS rn
  FROM (
    SELECT t.id,
           n.name,
           (lower(n.name) = lower(:text)) AS is_exact,
           (n.name ILIKE :prefix ESCAPE '\') AS is_prefix
    FROM tags t
    JOIN tag_name n ON n.tag_id = t.id AND n.type = 'standard'
    WHERE n.name ILIKE :pattern ESCAPE '\')sql";
  if (query.scope == TagSearchScope::Root) {
    sql_text += "\n      AND t.parent_id IS NULL";
  } else if (query.scope == TagSearchScope::Subtree) {
    if (query.direct_only) {
      sql_text += "\n      AND t.parent_id = CAST(:parent_id AS uuid)";
    } else {
      sql_text += "\n      AND t.id IN (SELECT id FROM subtree)";
    }
  }
  sql_text += R"sql(
  ) m
  ORDER BY is_exact DESC, is_prefix DESC, length(name), name, id
  LIMIT :limit
),
ancestors(tag_id, cur_id, distance) AS (
  SELECT c.id, c.id, 0 FROM candidates c
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
  LEFT JOIN tag_name n ON n.tag_id = a.cur_id AND n.type = 'standard'
),
aggregated AS (
  SELECT tag_id,
         string_agg(segment, '::' ORDER BY distance DESC) AS path
  FROM segments
  GROUP BY tag_id
)
SELECT c.id::text, c.name, ag.path
FROM candidates c
JOIN aggregated ag ON ag.tag_id = c.id
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

Result<DeleteArtworkResult, TagimgRepositoryError>
TagimgRepository::delete_artwork(const std::string &artwork_id) const {
  if (!is_uuid(artwork_id)) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());
    soci::transaction transaction(sql);

    // 先记下这个作品的图片，删除后再判断哪些失去了全部引用。
    std::vector<std::string> image_ids;
    std::string id_value = artwork_id;
    {
      soci::rowset<soci::row> image_rows =
          (sql.prepare << kArtworkImageIdsSql, soci::use(id_value));
      for (const soci::row &row : image_rows) {
        image_ids.push_back(row.get<std::string>(0));
      }
    }

    // rowset 用完即销毁，避免提交事务时还留着未关闭的游标。
    bool deleted = false;
    {
      soci::rowset<soci::row> deleted_rows =
          (sql.prepare << kDeleteArtworkSql, soci::use(id_value));
      for (const soci::row &row : deleted_rows) {
        (void)row;
        deleted = true;
      }
    }

    DeleteArtworkResult result;
    result.deleted = deleted;
    if (deleted && !image_ids.empty()) {
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

Result<bool, TagimgRepositoryError>
TagimgRepository::update_artwork(const std::string &artwork_id,
                                 const ArtworkUpdate &update) const {
  if (!is_uuid(artwork_id)) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (!update.title && !update.published_at && !update.clear_published_at &&
      update.add_tag_ids.empty() && update.remove_tag_ids.empty() &&
      !update.image_ids) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (update.clear_published_at && update.published_at) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (update.title && validate_title(*update.title)) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (update.published_at && validate_published_at(*update.published_at)) {
    return TagimgRepositoryError::InvalidArgument;
  }
  if (update.image_ids && validate_artwork_image_order(*update.image_ids)) {
    return TagimgRepositoryError::InvalidArgument;
  }

  // 同一请求里的标签不能重复，也不能同时出现在添加和移除列表里。
  std::set<std::string> unique_tags;
  for (const std::string &id : update.add_tag_ids) {
    if (!is_uuid(id) || !unique_tags.insert(id).second) {
      return TagimgRepositoryError::InvalidArgument;
    }
  }
  for (const std::string &id : update.remove_tag_ids) {
    if (!is_uuid(id) || !unique_tags.insert(id).second) {
      return TagimgRepositoryError::InvalidArgument;
    }
  }
  if (unique_tags.size() > kMaxArtworkEditTags) {
    return TagimgRepositoryError::InvalidArgument;
  }

  if (!impl_ || !impl_->pool) {
    return TagimgRepositoryError::ConnectionFailed;
  }

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());
    soci::transaction transaction(sql);

    std::string id_value = artwork_id;
    bool found = false;
    {
      soci::rowset<soci::row> rows =
          (sql.prepare << kLockArtworkSql, soci::use(id_value));
      for (const soci::row &row : rows) {
        (void)row;
        found = true;
      }
    }
    if (!found) {
      return false;
    }

    // 添加和移除引用到的标签都必须存在，避免部分生效。
    if (!unique_tags.empty()) {
      const std::vector<std::string> all_tags(unique_tags.begin(),
                                              unique_tags.end());
      const std::string tag_array = uuid_array_literal(all_tags);
      long long known = 0;
      sql << kCountExistingTagsSql, soci::use(tag_array), soci::into(known);
      if (known != static_cast<long long>(all_tags.size())) {
        return TagimgRepositoryError::UnknownTag;
      }
    }

    // 图片排序必须是当前作品图片集合的一个完整排列，不能借排序增删图片。
    if (update.image_ids) {
      std::set<std::string> current_image_ids;
      {
        soci::rowset<soci::row> rows =
            (sql.prepare << kLockArtworkImagesSql, soci::use(id_value));
        for (const soci::row &row : rows) {
          current_image_ids.insert(row.get<std::string>(0));
        }
      }
      if (current_image_ids.size() != update.image_ids->size()) {
        return TagimgRepositoryError::InvalidArgument;
      }
      for (const std::string &image_id : *update.image_ids) {
        if (current_image_ids.find(image_id) == current_image_ids.end()) {
          return TagimgRepositoryError::InvalidArgument;
        }
      }
    }

    if (update.title) {
      std::string title = *update.title;
      sql << kUpdateArtworkTitleSql, soci::use(title), soci::use(id_value);
    }
    if (update.published_at) {
      std::string published_at = *update.published_at;
      sql << kUpdateArtworkPublishedAtSql, soci::use(published_at),
          soci::use(id_value);
    }
    if (update.clear_published_at) {
      sql << kClearArtworkPublishedAtSql, soci::use(id_value);
    }
    if (!update.add_tag_ids.empty()) {
      const std::string tag_array = uuid_array_literal(update.add_tag_ids);
      sql << kInsertArtworkTagsSql, soci::use(id_value), soci::use(tag_array);
    }
    if (!update.remove_tag_ids.empty()) {
      const std::string tag_array = uuid_array_literal(update.remove_tag_ids);
      sql << kDeleteArtworkTagsSql, soci::use(id_value), soci::use(tag_array);
    }
    if (update.image_ids) {
      const std::string image_array = uuid_array_literal(*update.image_ids);
      sql << kUpdateArtworkImagePositionsSql, soci::use(image_array),
          soci::use(id_value);
    }

    sql << kTouchArtworkSql, soci::use(id_value);
    transaction.commit();
    return true;
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

} // namespace tagimg
