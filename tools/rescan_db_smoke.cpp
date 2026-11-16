// 作品写入事务对象的数据库冒烟测试。
//
// 只碰自建的临时数据（固定 UUID + smoke_ 前缀的标签名），跑完全部删除，
// 不影响现有作品。覆盖：
//   1. 事务对象能一次写入标题、向量、扫描标签与 scan_at；
//   2. 扫描标签只增不删，重复添加不会重复挂载，手动标签不受影响；
//   3. 不 commit 直接析构时整单回滚，旧向量保持原样；
//   4. 维度不符 / 作品不存在等失败路径；
//   5. 向量按 image_id 替换而不是新增。
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <soci/postgresql/soci-postgresql.h>
#include <soci/soci.h>

#include "common/config.hpp"
#include "db/postgres_util.hpp"
#include "db/tagimg_repository.hpp"

namespace {

constexpr std::size_t kDim = 768;

// 用 std::string 而不是 const char*：soci 的 use() 不接受 const char*。
const std::string kArtworkId = "7ec5a000-0000-4000-8000-000000000001";
const std::string kMissingArtworkId = "7ec5a000-0000-4000-8000-0000000000ff";
const std::string kImageA = "7ec5a000-0000-4000-8000-000000000011";
const std::string kImageB = "7ec5a000-0000-4000-8000-000000000012";
const std::string kCategoryName = "smokecat";
const std::string kTagAlpha = "smoke_tag_alpha";
const std::string kTagBeta = "smoke_tag_beta";
const std::string kTagManual = "smoke_tag_manual";

// 造一个确定性的单位向量，便于核对写入和读回是否一致。
std::vector<float> make_vector(std::size_t seed) {
  std::vector<float> values(kDim, 0.0F);
  for (std::size_t index = 0; index < kDim; ++index) {
    values[index] = static_cast<float>((index * 31 + seed * 17) % 200 - 100);
  }
  double squared = 0.0;
  for (float value : values) {
    squared += static_cast<double>(value) * static_cast<double>(value);
  }
  const double length = squared > 0.0 ? std::sqrt(squared) : 1.0;
  for (float &value : values) {
    value = static_cast<float>(static_cast<double>(value) / length);
  }
  return values;
}

std::string uuid_array_literal(const std::vector<std::string> &ids) {
  std::string literal = "{";
  for (std::size_t index = 0; index < ids.size(); ++index) {
    if (index != 0) {
      literal.push_back(',');
    }
    literal += ids[index];
  }
  literal.push_back('}');
  return literal;
}

struct Checks {
  int passed = 0;
  int failed = 0;

  void check(const std::string &name, bool ok) {
    if (ok) {
      ++passed;
    } else {
      ++failed;
      std::cerr << "FAIL " << name << '\n';
    }
  }
};

// 删除本轮造出来的全部临时数据；顺序按外键依赖。
void cleanup(soci::session &sql) {
  sql << "DELETE FROM artworks WHERE id = CAST(:id AS uuid)", soci::use(kArtworkId);
  const std::string images = uuid_array_literal({kImageA, kImageB});
  sql << "DELETE FROM images WHERE id = ANY(CAST(:ids AS uuid[]))",
      soci::use(images);
  sql << "DELETE FROM tags WHERE id IN ("
         "  SELECT tag_id FROM tag_name WHERE name IN (:a, :b, :c, :d))",
      soci::use(kTagAlpha), soci::use(kTagBeta), soci::use(kTagManual),
      soci::use(kCategoryName);
}

std::optional<std::string> find_root_tag(soci::session &sql) {
  soci::rowset<soci::row> rows =
      (sql.prepare << "SELECT t.id::text FROM tags t "
                      "JOIN tag_name n ON n.tag_id = t.id "
                      "WHERE n.type = 'standard' AND n.name = 'wd14scan' "
                      "  AND t.parent_id IS NULL LIMIT 1");
  const auto row = rows.begin();
  if (row == rows.end()) {
    return std::nullopt;
  }
  return row->get<std::string>(0);
}

std::string find_or_create_category(soci::session &sql,
                                    const std::string &root_id) {
  soci::rowset<soci::row> rows =
      (sql.prepare << "SELECT t.id::text FROM tags t "
                      "JOIN tag_name n ON n.tag_id = t.id "
                      "WHERE n.type = 'standard' AND n.name = :name "
                      "  AND t.parent_id = CAST(:parent AS uuid) LIMIT 1",
       soci::use(kCategoryName), soci::use(root_id));
  const auto row = rows.begin();
  if (row != rows.end()) {
    return row->get<std::string>(0);
  }
  std::string created;
  sql << "INSERT INTO tags (parent_id) VALUES (CAST(:parent AS uuid)) "
         "RETURNING id::text",
      soci::into(created), soci::use(root_id);
  sql << "INSERT INTO tag_name (tag_id, type, name) "
         "VALUES (CAST(:id AS uuid), 'standard'::tag_name_type, :name)",
      soci::use(created), soci::use(kCategoryName);
  return created;
}

// 只带一个参数的作品级计数。
long long count_scalar(soci::session &sql, const std::string &statement,
                       const std::string &id) {
  long long value = 0;
  soci::statement st = (sql.prepare << statement, soci::into(value),
                        soci::use(id));
  st.execute(true);
  return value;
}

long long count_scalar2(soci::session &sql, const std::string &statement,
                        const std::string &first, const std::string &second) {
  long long value = 0;
  soci::statement st = (sql.prepare << statement, soci::into(value),
                        soci::use(first), soci::use(second));
  st.execute(true);
  return value;
}

std::string embedding_text(soci::session &sql, const std::string &image_id) {
  std::string text;
  soci::indicator indicator = soci::i_null;
  sql << "SELECT embedding::text FROM chinese_clip_vit_large_patch14 "
         "WHERE image_id = CAST(:id AS uuid)",
      soci::into(text, indicator), soci::use(image_id);
  return indicator == soci::i_ok ? text : std::string();
}

} // namespace

int main(int argc, char **argv) {
  std::filesystem::path config_path = "config.json";
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--config" && index + 1 < argc) {
      config_path = argv[++index];
    } else {
      std::cerr << "usage: " << argv[0] << " [--config PATH]\n";
      return 2;
    }
  }

  const auto config = tagimg::load_config(config_path);
  if (!config) {
    std::cerr << "config error: " << tagimg::to_string(config.error()) << '\n';
    return 2;
  }

  tagimg::TagimgDbConfig db;
  db.host = config->database.host;
  db.port = config->database.port;
  db.user = config->database.user;
  db.password = config->database.password;
  db.dbname = config->database.image_dbname;

  auto repository = tagimg::TagimgRepository::connect(db);
  if (!repository) {
    std::cerr << "connect error: " << tagimg::to_string(repository.error())
              << '\n';
    return 2;
  }

  soci::session sql(
      soci::postgresql,
      tagimg::db_detail::make_conninfo(db.host, db.port, db.dbname, db.user,
                                       db.password));

  nlohmann::json output;
  Checks checks;

  cleanup(sql);

  const auto root_id = find_root_tag(sql);
  if (!root_id) {
    output["skipped"] = "root tag wd14scan not found";
    std::cout << output.dump(2) << '\n';
    return 0;
  }
  const std::string category_id = find_or_create_category(sql, *root_id);
  output["category_id"] = category_id;

  // 临时作品：两张没有 entity_id 的图片，模拟作品内的图片。
  sql << "INSERT INTO artworks (id, title) "
         "VALUES (CAST(:id AS uuid), 'smoke rescan')",
      soci::use(kArtworkId);
  const std::string images = uuid_array_literal({kImageA, kImageB});
  sql << "INSERT INTO images (id) "
         "SELECT unnest(CAST(:ids AS uuid[]))",
      soci::use(images);
  sql << "INSERT INTO artwork_images (artwork_id, image_id, position) "
         "SELECT CAST(:artwork AS uuid), t.id, (row_number() OVER ()) - 1 "
         "FROM (SELECT unnest(CAST(:ids AS uuid[])) AS id) t",
      soci::use(kArtworkId), soci::use(images);

  const std::vector<float> vector_a = make_vector(1);
  const std::vector<float> vector_b = make_vector(2);

  // 1. 正常路径：一次事务写入标题、向量、扫描标签与 scan_at。
  {
    auto opened = (*repository)->begin_artwork_update(kArtworkId);
    checks.check("begin_artwork_update 成功", opened.has_value());
    checks.check("作品存在", opened && opened->has_value());
    if (!opened || !opened->has_value()) {
      cleanup(sql);
      std::cout << output.dump(2) << '\n';
      return 1;
    }

    tagimg::ArtworkUpdateTransaction &txn = **opened;
    const auto embeddings = txn.replace_embeddings(
        {{kImageA, vector_a}, {kImageB, vector_b}});
    checks.check("写入两条向量", embeddings && *embeddings == 2);
    checks.check(
        "挂载两条扫描标签",
        txn.add_scanned_tags({{kTagAlpha, kCategoryName},
                              {kTagBeta, kCategoryName}})
                .value_or(0) == 2);
    checks.check("更新两张图片的 scan_at",
                 txn.touch_image_scans({kImageA, kImageB}).value_or(0) == 2);
    checks.check("设置标题", static_cast<bool>(txn.set_title("smoke rescanned")));
    checks.check("提交成功", static_cast<bool>(txn.commit()));
  }

  checks.check(
      "向量表有两条",
      count_scalar(sql,
                   "SELECT count(*) FROM chinese_clip_vit_large_patch14 "
                   "WHERE image_id = ANY(CAST(:ids AS uuid[]))",
                   images) == 2);
  checks.check(
      "向量维度为 768",
      count_scalar(sql,
                   "SELECT count(*) FROM chinese_clip_vit_large_patch14 "
                   "WHERE image_id = ANY(CAST(:ids AS uuid[])) "
                   "  AND vector_dims(embedding) = 768",
                   images) == 2);
  checks.check(
      "扫描标签挂到作品上",
      count_scalar(sql,
                   "SELECT count(*) FROM artwork_tags "
                   "WHERE artwork_id = CAST(:id AS uuid)",
                   kArtworkId) == 2);
  checks.check(
      "scan_at 已更新",
      count_scalar(sql,
                   "SELECT count(*) FROM images "
                   "WHERE id = ANY(CAST(:ids AS uuid[])) "
                   "  AND scan_at IS NOT NULL",
                   images) == 2);
  {
    std::string title;
    sql << "SELECT title FROM artworks WHERE id = CAST(:id AS uuid)",
        soci::into(title), soci::use(kArtworkId);
    checks.check("标题已改写", title == "smoke rescanned");
  }

  // 2. 只增不删：手动标签不受影响，重复添加不会重复挂载。
  {
    std::string manual_id;
    sql << "INSERT INTO tags (parent_id) VALUES (CAST(:parent AS uuid)) "
           "RETURNING id::text",
        soci::into(manual_id), soci::use(*root_id);
    sql << "INSERT INTO tag_name (tag_id, type, name) "
           "VALUES (CAST(:id AS uuid), 'standard'::tag_name_type, :name)",
        soci::use(manual_id), soci::use(kTagManual);
    sql << "INSERT INTO artwork_tags (artwork_id, tag_id) "
           "VALUES (CAST(:artwork AS uuid), CAST(:tag AS uuid))",
        soci::use(kArtworkId), soci::use(manual_id);

    auto opened = (*repository)->begin_artwork_update(kArtworkId);
    if (opened && opened->has_value()) {
      tagimg::ArtworkUpdateTransaction &txn = **opened;
      (void)txn.add_scanned_tags(
          {{kTagAlpha, kCategoryName}, {kTagBeta, kCategoryName}});
      (void)txn.commit();
    }
    checks.check(
        "重复添加不产生重复行",
        count_scalar(sql,
                     "SELECT count(*) FROM artwork_tags "
                     "WHERE artwork_id = CAST(:id AS uuid)",
                     kArtworkId) == 3);
    checks.check(
        "手动标签仍然存在",
        count_scalar2(sql,
                      "SELECT count(*) FROM artwork_tags "
                      "WHERE artwork_id = CAST(:id AS uuid) "
                      "  AND tag_id = CAST(:tag AS uuid)",
                      kArtworkId, manual_id) != 0);
  }

  // 3. 不 commit 就析构：整单回滚，旧向量保持原样。
  const std::string before = embedding_text(sql, kImageA);
  checks.check("读回旧向量", !before.empty());
  {
    auto opened = (*repository)->begin_artwork_update(kArtworkId);
    if (opened && opened->has_value()) {
      tagimg::ArtworkUpdateTransaction txn = std::move(**opened);
      (void)txn.replace_embeddings({{kImageA, make_vector(9)}});
      (void)txn.set_title("should roll back");
    }
  }
  checks.check("未提交时向量不变", embedding_text(sql, kImageA) == before);
  {
    std::string title;
    sql << "SELECT title FROM artworks WHERE id = CAST(:id AS uuid)",
        soci::into(title), soci::use(kArtworkId);
    checks.check("未提交时标题不变", title == "smoke rescanned");
  }

  // 4. 维度不符被拒绝，事务随后回滚，向量不变。
  {
    auto opened = (*repository)->begin_artwork_update(kArtworkId);
    if (opened && opened->has_value()) {
      tagimg::ArtworkUpdateTransaction txn = std::move(**opened);
      const auto bad = txn.replace_embeddings({{kImageA, {1.0F, 2.0F, 3.0F}}});
      checks.check("维度不符返回错误",
                   !bad &&
                       bad.error() ==
                           tagimg::TagimgRepositoryError::InvalidArgument);
    }
  }
  checks.check("失败后向量不变", embedding_text(sql, kImageA) == before);

  // 5. 向量按 image_id 替换，不会新增行。
  {
    auto opened = (*repository)->begin_artwork_update(kArtworkId);
    if (opened && opened->has_value()) {
      tagimg::ArtworkUpdateTransaction &txn = **opened;
      (void)txn.replace_embeddings({{kImageA, make_vector(5)}});
      (void)txn.commit();
    }
    checks.check("替换后仍只有一条",
                 count_scalar(sql,
                              "SELECT count(*) FROM chinese_clip_vit_large_patch14 "
                              "WHERE image_id = CAST(:id AS uuid)",
                              kImageA) == 1);
    checks.check("替换后值已变化", embedding_text(sql, kImageA) != before);
  }

  // 6. 作品不存在时 begin 返回空 optional，而不是错误。
  {
    auto opened = (*repository)->begin_artwork_update(kMissingArtworkId);
    checks.check("不存在时查询成功", opened.has_value());
    checks.check("不存在时返回空",
                 opened && !opened->has_value());
    checks.check("非 uuid 被拒绝",
                 !(*repository)->begin_artwork_update("not-a-uuid"));
  }

  cleanup(sql);

  output["checks_passed"] = checks.passed;
  output["checks_failed"] = checks.failed;
  std::cout << output.dump(2) << '\n';
  return checks.failed == 0 ? 0 : 1;
}
