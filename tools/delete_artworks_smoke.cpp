// 批量删除作品的数据库冒烟测试。
//
// 只碰自建的临时数据（固定 UUID），跑完全部删除，不影响现有作品。覆盖：
//   1. 一次删除多个作品，删除在单事务内完成；
//   2. 只属于被删作品的图片变成游离图片并计入数量；
//   3. 与未删除作品共用的图片保持关联，不变成游离图片；
//   4. 不存在的作品 ID 忽略，全部不存在时 deleted=false；
//   5. 空数组返回 InvalidArgument；
//   6. 混入非法 UUID 时整单失败，合法作品保持不变；
//   7. 重复 ID 去重后只删除一次。
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
const std::string kArtworkA = "7ec5b000-0000-4000-8000-000000000001";
const std::string kArtworkB = "7ec5b000-0000-4000-8000-000000000002";
const std::string kArtworkC = "7ec5b000-0000-4000-8000-000000000003";
const std::string kMissingArtwork = "7ec5b000-0000-4000-8000-0000000000ff";
const std::string kImageExclusiveA = "7ec5b000-0000-4000-8000-000000000011";
const std::string kImageExclusiveB = "7ec5b000-0000-4000-8000-000000000012";
const std::string kImageShared = "7ec5b000-0000-4000-8000-000000000013";
const std::string kImageOnlyC = "7ec5b000-0000-4000-8000-000000000014";

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

// 删除本轮造出来的全部临时数据；artwork_images 由外键级联清理。
void cleanup(soci::session &sql) {
  const std::string artworks =
      uuid_array_literal({kArtworkA, kArtworkB, kArtworkC});
  sql << "DELETE FROM artworks WHERE id = ANY(CAST(:ids AS uuid[]))",
      soci::use(artworks);
  const std::string images = uuid_array_literal(
      {kImageExclusiveA, kImageExclusiveB, kImageShared, kImageOnlyC});
  sql << "DELETE FROM images WHERE id = ANY(CAST(:ids AS uuid[]))",
      soci::use(images);
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

  // A、B 将被删除；C 保留并共用 kImageShared。
  insert_artwork(sql, kArtworkA, "smoke delete A");
  insert_artwork(sql, kArtworkB, "smoke delete B");
  insert_artwork(sql, kArtworkC, "smoke keep C");
  insert_image(sql, kImageExclusiveA);
  insert_image(sql, kImageExclusiveB);
  insert_image(sql, kImageShared);
  insert_image(sql, kImageOnlyC);
  attach(sql, kArtworkA, kImageExclusiveA, 0);
  attach(sql, kArtworkA, kImageShared, 1);
  attach(sql, kArtworkB, kImageExclusiveB, 0);
  attach(sql, kArtworkB, kImageShared, 1);
  attach(sql, kArtworkC, kImageShared, 0);
  attach(sql, kArtworkC, kImageOnlyC, 1);

  // 1. 批量删除 A、B：两个独占图片变游离，共用图片仍属于 C。
  {
    const auto result = (*repository)->delete_artworks({kArtworkA, kArtworkB});
    checks.check("批量删除成功", result.has_value());
    if (result) {
      checks.check("deleted=true", result->deleted);
      checks.check("游离图片数量=2", result->newly_orphaned_image_count == 2);
    }

    checks.check("作品 A 已删除", artwork_count(sql, kArtworkA) == 0);
    checks.check("作品 B 已删除", artwork_count(sql, kArtworkB) == 0);
    checks.check("作品 C 保留", artwork_count(sql, kArtworkC) == 1);

    checks.check("独占图片 A 行保留", image_count(sql, kImageExclusiveA) == 1);
    checks.check("独占图片 B 行保留", image_count(sql, kImageExclusiveB) == 1);
    checks.check("共用图片行保留", image_count(sql, kImageShared) == 1);
    checks.check("C 独占图片行保留", image_count(sql, kImageOnlyC) == 1);

    checks.check("独占图片 A 变游离", attach_count(sql, kImageExclusiveA) == 0);
    checks.check("独占图片 B 变游离", attach_count(sql, kImageExclusiveB) == 0);
    checks.check("共用图片仍关联 C",
                 attach_exists(sql, kArtworkC, kImageShared));
    checks.check("共用图片只关联 1 个作品",
                 attach_count(sql, kImageShared) == 1);
    checks.check("C 独占图片仍关联",
                 attach_exists(sql, kArtworkC, kImageOnlyC));
  }

  // 2. 全部不存在的 ID：幂等成功，deleted=false。
  {
    const auto result = (*repository)->delete_artworks({kMissingArtwork});
    checks.check("缺失作品不报错", result.has_value());
    if (result) {
      checks.check("缺失作品 deleted=false", !result->deleted);
      checks.check("缺失作品游离数量=0",
                   result->newly_orphaned_image_count == 0);
    }
    checks.check("作品 C 未受影响", artwork_count(sql, kArtworkC) == 1);
  }

  // 3. 空数组为非法参数。
  {
    const auto result = (*repository)->delete_artworks({});
    checks.check("空数组返回 InvalidArgument",
                 !result && result.error() ==
                                tagimg::TagimgRepositoryError::InvalidArgument);
  }

  // 4. 混入非法 UUID：整单失败，合法作品 C 保持原样。
  {
    const auto result =
        (*repository)->delete_artworks({kArtworkC, "not-a-uuid"});
    checks.check("非法 UUID 返回 InvalidArgument",
                 !result && result.error() ==
                                tagimg::TagimgRepositoryError::InvalidArgument);
    checks.check("非法 UUID 不删除作品 C", artwork_count(sql, kArtworkC) == 1);
    checks.check("非法 UUID 不破坏图片关联",
                 attach_exists(sql, kArtworkC, kImageShared) &&
                     attach_exists(sql, kArtworkC, kImageOnlyC));
  }

  // 5. 重复 ID 去重后删除 C；此时共用图片和 C 的独占图片都变游离。
  {
    const auto result = (*repository)->delete_artworks({kArtworkC, kArtworkC});
    checks.check("重复 ID 删除成功", result.has_value());
    if (result) {
      checks.check("重复 ID deleted=true", result->deleted);
      checks.check("重复 ID 游离数量=2",
                   result->newly_orphaned_image_count == 2);
    }
    checks.check("作品 C 已删除", artwork_count(sql, kArtworkC) == 0);
    checks.check("共用图片变游离", attach_count(sql, kImageShared) == 0);
    checks.check("C 独占图片变游离", attach_count(sql, kImageOnlyC) == 0);
    checks.check("全部图片行仍保留", image_count(sql, kImageShared) == 1 &&
                                         image_count(sql, kImageOnlyC) == 1);
  }

  cleanup(sql);

  std::cout << "passed " << checks.passed << ", failed " << checks.failed
            << '\n';
  return checks.failed == 0 ? 0 : 1;
}
