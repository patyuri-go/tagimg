#include "db/entity_repository.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <utility>

#include <soci/connection-pool.h>
#include <soci/postgresql/soci-postgresql.h>
#include <soci/soci.h>

#include "common/uuid.hpp"
#include "db/postgres_util.hpp"

namespace tagimg {
namespace {

constexpr const char *kFindLocationsSql = R"sql(
SELECT l.storage_id::text,
       s.name,
       s.endpoint,
       s.config->>'base_path',
       s.config->>'username',
       s.config->>'password',
       l.key
FROM entity_locations l
JOIN storage_nodes s ON s.storage_id = l.storage_id
WHERE l.entity_id = :entity_id
  AND lower(l.state) = 'available'
  AND lower(s.state) = 'active'
  AND lower(s.type) = 'webdav'
ORDER BY l.verified_at DESC NULLS LAST, s.name
)sql";

constexpr const char *kFindEntitySql = R"sql(
SELECT mime_type, size
FROM entities
WHERE entity_id = :entity_id
)sql";

bool is_ascii_space(char c) {
  return std::isspace(static_cast<unsigned char>(c)) != 0;
}

std::string trim(std::string value) {
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(),
                           [](char c) { return !is_ascii_space(c); }));
  value.erase(std::find_if(value.rbegin(), value.rend(),
                           [](char c) { return !is_ascii_space(c); })
                  .base(),
              value.end());
  return value;
}

// RFC 3986 的 unreserved 集合：ALPHA / DIGIT / "-" / "." / "_" / "~"。
bool is_unreserved(unsigned char c) {
  return std::isalnum(c) != 0 || c == '-' || c == '.' || c == '_' || c == '~';
}

// 按 RFC 3986 做百分号编码，非 unreserved 字节（含 UTF-8 多字节）写成 %XX。
//
// 这里没有用 drogon::utils::urlEncode / urlEncodeComponent：它们是表单和
// 查询串编码器，会把空格写成 '+'，urlEncode 还会原样保留 '/', '?', '&',
// '=' 等字符。用于 WebDAV 路径段或 URL userinfo 会拼出错误的直链。
std::string percent_encode(std::string_view value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(value.size());
  for (const unsigned char c : value) {
    if (is_unreserved(c)) {
      encoded.push_back(static_cast<char>(c));
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[c >> 4]);
      encoded.push_back(kHex[c & 0x0F]);
    }
  }
  return encoded;
}

// 路径按 '/' 分段编码，段与段之间保留一个 '/'；开头的 '/' 去掉，
// 末尾的 '/' 保留（由调用方决定是否补前导斜杠）。
std::string encode_path(std::string_view path) {
  std::string encoded;
  encoded.reserve(path.size());

  std::size_t begin = 0;
  while (begin < path.size()) {
    const std::size_t slash = path.find('/', begin);
    const std::size_t end =
        slash == std::string_view::npos ? path.size() : slash;

    if (end > begin) {
      if (!encoded.empty() && encoded.back() != '/') {
        encoded.push_back('/');
      }
      encoded += percent_encode(path.substr(begin, end - begin));
    }
    if (slash == std::string_view::npos) {
      break;
    }
    if (!encoded.empty() && encoded.back() != '/') {
      encoded.push_back('/');
    }
    begin = slash + 1;
  }

  return encoded;
}

std::string null_as_empty(const soci::row &row, std::size_t index) {
  return row.get<std::string>(index, std::string{});
}

} // namespace

std::string_view to_string(EntityRepositoryError error) noexcept {
  switch (error) {
  case EntityRepositoryError::InvalidEntityId:
    return "InvalidEntityId";
  case EntityRepositoryError::ConnectionFailed:
    return "ConnectionFailed";
  case EntityRepositoryError::QueryFailed:
    return "QueryFailed";
  case EntityRepositoryError::NotFound:
    return "NotFound";
  }
  return "Unknown";
}

std::string build_webdav_url(std::string_view endpoint,
                             std::string_view base_path, std::string_view key,
                             std::string_view username,
                             std::string_view password) {
  std::string authority = trim(std::string(endpoint));
  while (!authority.empty() && authority.back() == '/') {
    authority.pop_back();
  }
  if (authority.empty()) {
    return {};
  }

  if (authority.find("://") == std::string::npos) {
    authority.insert(0, "http://");
  }

  if (!username.empty()) {
    std::string userinfo = percent_encode(username);
    userinfo.push_back(':');
    userinfo += percent_encode(password);
    userinfo.push_back('@');
    const auto scheme_end = authority.find("://");
    authority.insert(scheme_end + 3, userinfo);
  }

  std::string path;
  const std::string encoded_base = encode_path(base_path);
  const std::string encoded_key = encode_path(key);
  if (!encoded_base.empty()) {
    path.push_back('/');
    path += encoded_base;
  }
  if (!encoded_key.empty()) {
    if (path.empty() || path.back() != '/') {
      path.push_back('/');
    }
    path += encoded_key;
  }

  return authority + path;
}

struct EntityRepository::Impl {
  EntityDbConfig config;
  std::unique_ptr<soci::connection_pool> pool;
};

EntityRepository::EntityRepository(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {
}

EntityRepository::~EntityRepository() = default;

Result<std::unique_ptr<EntityRepository>, EntityRepositoryError>
EntityRepository::connect(EntityDbConfig config, std::size_t pool_size) {
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
    return EntityRepositoryError::ConnectionFailed;
  }

  return std::unique_ptr<EntityRepository>(
      new EntityRepository(std::move(impl)));
}

Result<std::vector<WebdavLocation>, EntityRepositoryError>
EntityRepository::find_webdav_locations(std::string_view entity_id) const {
  if (!is_uuid(entity_id)) {
    return EntityRepositoryError::InvalidEntityId;
  }
  if (!impl_ || !impl_->pool) {
    return EntityRepositoryError::ConnectionFailed;
  }

  std::vector<WebdavLocation> locations;
  const std::string id(entity_id);

  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());
    soci::rowset<soci::row> rows =
        (sql.prepare << kFindLocationsSql, soci::use(id));

    for (const auto &row : rows) {
      const std::string key = null_as_empty(row, 6);
      if (key.empty()) {
        continue;
      }

      WebdavLocation location;
      location.storage_id = null_as_empty(row, 0);
      location.storage_name = null_as_empty(row, 1);
      location.url =
          build_webdav_url(null_as_empty(row, 2),      // 存储节点地址
                           null_as_empty(row, 3),      // 配置里的 base_path
                           key, null_as_empty(row, 4), // 配置里的 username
                           null_as_empty(row, 5));     // 配置里的 password
      if (location.url.empty()) {
        continue;
      }
      locations.push_back(std::move(location));
    }
  } catch (const std::exception &) {
    return EntityRepositoryError::QueryFailed;
  }

  if (locations.empty()) {
    return EntityRepositoryError::NotFound;
  }
  return locations;
}

Result<EntityMetadata, EntityRepositoryError>
EntityRepository::find_entity(std::string_view entity_id) const {
  if (!is_uuid(entity_id)) {
    return EntityRepositoryError::InvalidEntityId;
  }
  if (!impl_ || !impl_->pool) {
    return EntityRepositoryError::ConnectionFailed;
  }

  const std::string id(entity_id);
  try {
    db_detail::PoolLease lease(*impl_->pool);
    soci::session &sql = impl_->pool->at(lease.position());
    soci::rowset<soci::row> rows =
        (sql.prepare << kFindEntitySql, soci::use(id));

    const auto row = rows.begin();
    if (row == rows.end()) {
      return EntityRepositoryError::NotFound;
    }

    EntityMetadata metadata;
    metadata.mime_type = null_as_empty(*row, 0);
    metadata.size = static_cast<std::uint64_t>(row->get<long long>(1));
    return metadata;
  } catch (const std::exception &) {
    return EntityRepositoryError::QueryFailed;
  }
}

} // namespace tagimg
