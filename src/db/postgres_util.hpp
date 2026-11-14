#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <soci/connection-pool.h>

namespace tagimg::db_detail {

// 把值包成 PostgreSQL conninfo 里的单引号字面量。
inline std::string escape_conninfo_value(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('\'');
  for (const char c : value) {
    if (c == '\\' || c == '\'') {
      escaped.push_back('\\');
    }
    escaped.push_back(c);
  }
  escaped.push_back('\'');
  return escaped;
}

inline std::string make_conninfo(std::string_view host, std::uint16_t port,
                                 std::string_view dbname, std::string_view user,
                                 std::string_view password) {
  std::string conninfo;
  conninfo += "host=" + escape_conninfo_value(host);
  conninfo += " port=" + std::to_string(port);
  conninfo += " dbname=" + escape_conninfo_value(dbname);
  conninfo += " user=" + escape_conninfo_value(user);
  conninfo += " password=" + escape_conninfo_value(password);
  conninfo += " connect_timeout=10";
  return conninfo;
}

// 离开作用域时把连接归还到连接池。
class PoolLease {
public:
  explicit PoolLease(soci::connection_pool &pool)
      : pool_(pool), position_(pool.lease()) {
  }

  ~PoolLease() {
    pool_.give_back(position_);
  }

  PoolLease(const PoolLease &) = delete;
  PoolLease &operator=(const PoolLease &) = delete;

  [[nodiscard]] std::size_t position() const {
    return position_;
  }

private:
  soci::connection_pool &pool_;
  std::size_t position_;
};

} // namespace tagimg::db_detail
