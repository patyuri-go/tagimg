#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace tagimg {

// 外部存储服务的地址。图片原始内容统一从
// {scheme}://{host}:{port}/api/entities/{entityId}/content 读取。
struct StorageConfig {
  std::string scheme = "http";
  std::string host;
  std::uint16_t port = 5666;
};

// 拼出实体内容的绝对 URL。entity_id 必须先通过 UUID 校验。
[[nodiscard]] std::string entity_content_url(const StorageConfig &storage,
                                             std::string_view entity_id);

} // namespace tagimg
