#include "common/storage.hpp"

#include <string>

namespace tagimg {

std::string entity_content_url(const StorageConfig &storage,
                               std::string_view entity_id) {
  std::string url;
  url.reserve(storage.scheme.size() + storage.host.size() + entity_id.size() +
              48);
  url += storage.scheme.empty() ? "http" : storage.scheme;
  url += "://";

  // IPv6 字面量需要方括号；配置里已经带括号时直接使用。
  const bool wrap_ipv6 = storage.host.find(':') != std::string::npos &&
                         (storage.host.empty() || storage.host.front() != '[');
  if (wrap_ipv6) {
    url += '[';
  }
  url += storage.host;
  if (wrap_ipv6) {
    url += ']';
  }

  url += ':';
  url += std::to_string(storage.port);
  url += "/api/entities/";
  url += entity_id;
  url += "/content";
  return url;
}

} // namespace tagimg
