#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/result.hpp"

namespace tagimg {

struct EntityDbConfig {
  std::string host;
  std::uint16_t port = 5432;
  std::string user;
  std::string password;
  std::string dbname;
};

enum class EntityRepositoryError {
  InvalidEntityId,
  ConnectionFailed,
  QueryFailed,
  NotFound,
};

[[nodiscard]] std::string_view to_string(EntityRepositoryError error) noexcept;

// 实体自身的基本信息，不包含位置记录。
struct EntityMetadata {
  std::string mime_type;
  std::uint64_t size = 0;
};

// 一个可解析的 WebDAV 文件位置。
struct WebdavLocation {
  std::string storage_id;
  std::string storage_name;
  // 带 user:password 的绝对 URL，可直接交给字节流客户端使用。
  std::string url;
};

// 用实体库里保存的信息拼出绝对 WebDAV URL。
//
// endpoint 可以不带协议头，缺省按 http:// 处理。base_path 与 key 之间
// 只保留一个斜杠。路径的每一段都做百分号编码，username/password 按
// URL userinfo 规则编码。
[[nodiscard]] std::string build_webdav_url(std::string_view endpoint,
                                           std::string_view base_path,
                                           std::string_view key,
                                           std::string_view username,
                                           std::string_view password);

// 实体库的只读访问入口。
//
// 对象内部维护一个小连接池，可被多线程调用。实例只能通过 connect() 创建；
// 只返回「位置状态为 available 且存储节点为 active WebDAV」的记录。
class EntityRepository {
public:
  static Result<std::unique_ptr<EntityRepository>, EntityRepositoryError>
  connect(EntityDbConfig config, std::size_t pool_size = 4);

  EntityRepository(const EntityRepository &) = delete;
  EntityRepository &operator=(const EntityRepository &) = delete;
  EntityRepository(EntityRepository &&) = delete;
  EntityRepository &operator=(EntityRepository &&) = delete;

  ~EntityRepository();

  // 返回 entity_id 的全部可用 WebDAV 位置，最近验证过的排在前面。
  // 没有任何可用位置时返回 NotFound。
  [[nodiscard]] Result<std::vector<WebdavLocation>, EntityRepositoryError>
  find_webdav_locations(std::string_view entity_id) const;

  // 返回实体的 mime_type 与字节大小；实体不存在时返回 NotFound。
  [[nodiscard]] Result<EntityMetadata, EntityRepositoryError>
  find_entity(std::string_view entity_id) const;

private:
  struct Impl;
  explicit EntityRepository(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

} // namespace tagimg
