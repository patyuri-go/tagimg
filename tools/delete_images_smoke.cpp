// 批量删除图片的数据库冒烟测试。
//
// 只碰自建的临时数据（固定 UUID），跑完全部删除，不影响现有数据。覆盖：
//   1. 删除作品内图片和游离图片，作品保留；
//   2. 作品失去图片后 updated_at 刷新，未受影响的作品不变；
//   3. image_tags 和 CLIP 向量随图片级联删除；
//   4. 不存在的图片 ID 忽略，并按请求顺序返回；
//   5. 空数组返回 InvalidArgument；
//   6. 混入非法 UUID 时整单失败，合法图片保持不变；
//   7. 重复 ID 去重；共用图片从所有作品移除；
//   8. 作品失去最后一张图片后变空，但作品行保留。
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <soci/postgresql/soci-postgresql.h>
#include <soci/soci.h>

#include "common/config.hpp"
#include "db/postgres_util.hpp"
#include "db/tagimg_repository.hpp"

namespace {

// 用 std::string 而不是 const char*：soci 的 use() 不接受 const char*。
const std::string kArtworkA = "7ec5b000-0000-4000-8000-000000000101";
const std::string kArtworkB = "7ec5b000-0000-4000-8000-000000000102";
const std::string kArtworkC = "7ec5b000-0000-4000-8000-000000000103";

const std::string kImageA1 = "7ec5b000-0000-4000-8000-000000000111";
const std::string kImageA2 = "7ec5b000-0000-4000-8000-000000000112";
const std::string kImageShared = "7ec5b000-0000-4000-8000-000000000113";
const std::string kImageB1 = "7ec5b000-0000-4000-8000-000000000114";
const std::string kImageC1 = "7ec5b000-0000-4000-8000-000000000115";
const std::string kImageOrphan = "7ec5b000-0000-4000-8000-000000000116";

const std::string kTag = "7ec5b000-0000-4000-8000-0000000001e1";
const std::string kMissingImage1 = "7ec5b000-0000-4000-8000-0000000001f1";
const std::string kMissingImage2 = "7ec5b000-0000-4000-8000-0000000001f2";
const std::string kOldTimestamp = "2000-01-01T00:00:00Z";

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

// 删除本轮造出来的全部临时数据；关联表由外键级联清理。
void cleanup(soci::session &sql) {
  const std::string artworks =
      uuid_array_literal({kArtworkA, kArtworkB, kArtworkC});
  sql << "DELETE FROM artworks WHERE id = ANY(CAST(:ids AS uuid[]))",
      soci::use(artworks);
  const std::string images =
      uuid_array_literal({kImageA1, kImageA2, kImageShared, kImageB1,
                          kImageC1, kImageOrphan});
  sql << "DELETE FROM images WHERE id = ANY(CAST(:ids AS uuid[]))",
      soci::use(images);
  const std::string tags = uuid_array_literal({kTag});
  sql << "DELETE FROM tags WHERE id = ANY(CAST(:ids AS uuid[]))",
      soci::use(tags);
}

void insert_artwork(soci::session &sql, const std::string &artwork_id,
                    const std::string &title) {
  sql << "INSERT INTO artworks (id, title) VALUES (CAST(:id AS uuid), :title)",
      soci::use(artwork_id), soci::use(title);
}

void insert_image(soci::session &sql, const std::string &image_id) {
  sql << "INSERT INTO images (id) VALUES (CAST(:id AS uuid))",
      soci::use(image_id);
}

void attach(soci::session &sql, const std::string &artwork_id,
            const std::string &image_id, int position) {
  sql << "INSERT INTO artwork_images (artwork_id, image_id, position) "
         "VALUES (CAST(:artwork AS uuid), CAST(:image AS uuid), :position)",
      soci::use(artwork_id), soci::use(image_id), soci::use(position);
}

void insert_tag(soci::session &sql, const std::string &tag_id) {
  sql << "INSERT INTO tags (id) VALUES (CAST(:id AS uuid))",
      soci::use(tag_id);
}

void tag_image(soci::session &sql, const std::string &image_id,
               const std::string &tag_id) {
  sql << "INSERT INTO image_tags (image_id, tag_id) "
         "VALUES (CAST(:image AS uuid), CAST(:tag AS uuid))",
      soci::use(image_id), soci::use(tag_id);
}

std::string zero_embedding() {
  std::string embedding = "[";
  for (int index = 0; index < 768; ++index) {
    if (index != 0) {
      embedding.push_back(',');
    }
    embedding.push_back('0');
  }
  embedding.push_back(']');
  return embedding;
}

void insert_clip(soci::session &sql, const std::string &image_id) {
  const std::string embedding = zero_embedding();
  sql << "INSERT INTO chinese_clip_vit_large_patch14 (image_id, embedding) "
         "VALUES (CAST(:image AS uuid), CAST(:embedding AS vector))",
      soci::use(image_id), soci::use(embedding);
}

long long count_scalar(soci::session &sql, const std::string &statement,
                       const std::string &id) {
  long long value = 0;
  soci::statement st =
      (sql.prepare << statement, soci::into(value), soci::use(id));
  st.execute(true);
  return value;
}

long long artwork_count(soci::session &sql, const std::string &artwork_id) {
  return count_scalar(
      sql, "SELECT count(*) FROM artworks WHERE id = CAST(:id AS uuid)",
      artwork_id);
}

long long image_count(soci::session &sql, const std::string &image_id) {
  return count_scalar(
      sql, "SELECT count(*) FROM images WHERE id = CAST(:id AS uuid)",
      image_id);
}

long long attach_count(soci::session &sql, const std::string &image_id) {
  return count_scalar(sql,
                      "SELECT count(*) FROM artwork_images "
                      "WHERE image_id = CAST(:id AS uuid)",
                      image_id);
}

bool attach_exists(soci::session &sql, const std::string &artwork_id,
                   const std::string &image_id) {
  long long value = 0;
  soci::statement st =
      (sql.prepare << "SELECT count(*) FROM artwork_images "
                      "WHERE artwork_id = CAST(:artwork AS uuid) "
                      "  AND image_id = CAST(:image AS uuid)",
       soci::into(value), soci::use(artwork_id), soci::use(image_id));
  st.execute(true);
  return value == 1;
}

long long image_tag_count(soci::session &sql, const std::string &image_id) {
  return count_scalar(
      sql, "SELECT count(*) FROM image_tags WHERE image_id = CAST(:id AS uuid)",
      image_id);
}

long long clip_count(soci::session &sql, const std::string &image_id) {
  return count_scalar(
      sql,
      "SELECT count(*) FROM chinese_clip_vit_large_patch14 "
      "WHERE image_id = CAST(:id AS uuid)",
      image_id);
}

void set_old_timestamp(soci::session &sql, const std::string &artwork_id) {
  sql << "UPDATE artworks SET updated_at = CAST(:baseline AS timestamptz) "
         "WHERE id = CAST(:id AS uuid)",
      soci::use(kOldTimestamp), soci::use(artwork_id);
}

bool updated_after_baseline(soci::session &sql,
                            const std::string &artwork_id) {
  long long value = 0;
  soci::statement st =
      (sql.prepare << "SELECT count(*) FROM artworks "
                      "WHERE id = CAST(:id AS uuid) "
                      "  AND updated_at > CAST(:baseline AS timestamptz)",
       soci::into(value), soci::use(artwork_id), soci::use(kOldTimestamp));
  st.execute(true);
  return value == 1;
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

  soci::session sql(soci::postgresql,
                    tagimg::db_detail::make_conninfo(
                        db.host, db.port, db.dbname, db.user, db.password));

  Checks checks;
  cleanup(sql);

  // A 有 A1/A2/shared；B 有 shared/B1；C 只有 C1；orphan 不属于作品。
  insert_artwork(sql, kArtworkA, "smoke image delete A");
  insert_artwork(sql, kArtworkB, "smoke image delete B");
  insert_artwork(sql, kArtworkC, "smoke image delete C");
  insert_image(sql, kImageA1);
  insert_image(sql, kImageA2);
  insert_image(sql, kImageShared);
  insert_image(sql, kImageB1);
  insert_image(sql, kImageC1);
  insert_image(sql, kImageOrphan);
  attach(sql, kArtworkA, kImageA1, 0);
  attach(sql, kArtworkA, kImageA2, 1);
  attach(sql, kArtworkA, kImageShared, 2);
  attach(sql, kArtworkB, kImageShared, 0);
  attach(sql, kArtworkB, kImageB1, 1);
  attach(sql, kArtworkC, kImageC1, 0);

  insert_tag(sql, kTag);
  for (const std::string &image_id :
       {kImageA1, kImageA2, kImageShared, kImageOrphan}) {
    tag_image(sql, image_id, kTag);
    insert_clip(sql, image_id);
  }

  set_old_timestamp(sql, kArtworkA);
  set_old_timestamp(sql, kArtworkB);
  set_old_timestamp(sql, kArtworkC);

  // 1. 删除作品内图片和游离图片：作品 A 保留并仍有 A2/shared。
  {
    const auto result =
        (*repository)->delete_images({kImageA1, kImageOrphan});
    checks.check("删除图片成功", result.has_value());
    if (result) {
      checks.check("deleted_count=2", result->deleted_count == 2);
      checks.check("missing_ids 为空", result->missing_ids.empty());
    }

    checks.check("A1 行已删除", image_count(sql, kImageA1) == 0);
    checks.check("游离图片行已删除", image_count(sql, kImageOrphan) == 0);
    checks.check("A2 行保留", image_count(sql, kImageA2) == 1);
    checks.check("共用图片行保留", image_count(sql, kImageShared) == 1);
    checks.check("作品 A 保留", artwork_count(sql, kArtworkA) == 1);
    checks.check("A2 仍关联 A", attach_exists(sql, kArtworkA, kImageA2));
    checks.check("共用图片仍关联 A",
                 attach_exists(sql, kArtworkA, kImageShared));
    checks.check("A1 关联已清理", attach_count(sql, kImageA1) == 0);

    checks.check("A1 标签级联删除", image_tag_count(sql, kImageA1) == 0);
    checks.check("游离图片标签级联删除",
                 image_tag_count(sql, kImageOrphan) == 0);
    checks.check("A2 标签保留", image_tag_count(sql, kImageA2) == 1);
    checks.check("A1 向量级联删除", clip_count(sql, kImageA1) == 0);
    checks.check("游离图片向量级联删除", clip_count(sql, kImageOrphan) == 0);
    checks.check("A2 向量保留", clip_count(sql, kImageA2) == 1);

    checks.check("作品 A 刷新 updated_at",
                 updated_after_baseline(sql, kArtworkA));
    checks.check("作品 B 未刷新",
                 !updated_after_baseline(sql, kArtworkB));
    checks.check("作品 C 未刷新",
                 !updated_after_baseline(sql, kArtworkC));
  }

  // 2. 不存在的图片：成功返回，missing_ids 按请求顺序返回。
  {
    const auto result =
        (*repository)->delete_images({kMissingImage1, kMissingImage2});
    checks.check("缺失图片不报错", result.has_value());
    if (result) {
      checks.check("缺失图片 deleted_count=0", result->deleted_count == 0);
      checks.check(
          "missing_ids 按请求顺序",
          result->missing_ids ==
              std::vector<std::string>{kMissingImage1, kMissingImage2});
    }
    checks.check("作品 A 未受影响", artwork_count(sql, kArtworkA) == 1);
    checks.check("A2 未受影响", image_count(sql, kImageA2) == 1);
  }

  // 3. 空数组为非法参数。
  {
    const auto result = (*repository)->delete_images({});
    checks.check("空数组返回 InvalidArgument",
                 !result && result.error() ==
                                tagimg::TagimgRepositoryError::InvalidArgument);
  }

  // 4. 混入非法 UUID：整单失败，合法图片 A2 保持原样。
  {
    const auto result =
        (*repository)->delete_images({kImageA2, "not-a-uuid"});
    checks.check("非法 UUID 返回 InvalidArgument",
                 !result && result.error() ==
                                tagimg::TagimgRepositoryError::InvalidArgument);
    checks.check("非法 UUID 不删除 A2", image_count(sql, kImageA2) == 1);
    checks.check("非法 UUID 不破坏 A2 关联",
                 attach_exists(sql, kArtworkA, kImageA2));
    checks.check("非法 UUID 不删除 A2 标签",
                 image_tag_count(sql, kImageA2) == 1);
    checks.check("非法 UUID 不删除 A2 向量", clip_count(sql, kImageA2) == 1);
  }

  // 5. 重复 ID 去重；共用图片从 A、B 两个作品中移除。
  {
    const auto result =
        (*repository)->delete_images({kImageShared, kImageShared});
    checks.check("重复 ID 删除成功", result.has_value());
    if (result) {
      checks.check("重复 ID deleted_count=1", result->deleted_count == 1);
      checks.check("重复 ID missing_ids 为空", result->missing_ids.empty());
    }
    checks.check("共用图片行已删除", image_count(sql, kImageShared) == 0);
    checks.check("共用图片关联已清理", attach_count(sql, kImageShared) == 0);
    checks.check("A2 仍关联 A", attach_exists(sql, kArtworkA, kImageA2));
    checks.check("B1 仍关联 B", attach_exists(sql, kArtworkB, kImageB1));
    checks.check("作品 A 再次刷新",
                 updated_after_baseline(sql, kArtworkA));
    checks.check("作品 B 刷新 updated_at",
                 updated_after_baseline(sql, kArtworkB));
    checks.check("作品 C 仍未刷新",
                 !updated_after_baseline(sql, kArtworkC));
    checks.check("共用图片标签级联删除",
                 image_tag_count(sql, kImageShared) == 0);
    checks.check("共用图片向量级联删除",
                 clip_count(sql, kImageShared) == 0);
  }

  // 6. 删除 B、C 的最后一张图片：作品行保留但图片列表变空。
  {
    const auto result =
        (*repository)->delete_images({kImageB1, kImageC1});
    checks.check("删除最后图片成功", result.has_value());
    if (result) {
      checks.check("最后图片 deleted_count=2", result->deleted_count == 2);
      checks.check("最后图片 missing_ids 为空", result->missing_ids.empty());
    }
    checks.check("作品 B 保留", artwork_count(sql, kArtworkB) == 1);
    checks.check("作品 C 保留", artwork_count(sql, kArtworkC) == 1);
    checks.check("B1 关联已清理", attach_count(sql, kImageB1) == 0);
    checks.check("C1 关联已清理", attach_count(sql, kImageC1) == 0);
    checks.check("B1 行已删除", image_count(sql, kImageB1) == 0);
    checks.check("C1 行已删除", image_count(sql, kImageC1) == 0);
    checks.check("作品 B 刷新 updated_at",
                 updated_after_baseline(sql, kArtworkB));
    checks.check("作品 C 刷新 updated_at",
                 updated_after_baseline(sql, kArtworkC));
    checks.check("作品 A 仍保留 A2",
                 attach_exists(sql, kArtworkA, kImageA2));
  }

  cleanup(sql);

  std::cout << "passed " << checks.passed << ", failed " << checks.failed
            << '\n';
  return checks.failed == 0 ? 0 : 1;
}
