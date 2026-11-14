#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tagimg {

// addArtwork 的输入限制，REST 与 GraphQL 两个入口共用。
inline constexpr std::size_t kMaxEntityIds = 20;
inline constexpr std::size_t kMaxImageIds = 20;
inline constexpr std::size_t kMaxTitleLength = 500;

// 标签搜索的关键词长度与返回条数上限，仓储层与 GraphQL 层共用。
inline constexpr std::size_t kMaxTagSearchLength = 100;
inline constexpr std::size_t kMaxTagSearchLimit = 50;
// 编辑作品时单次添加/移除的标签数量上限。
inline constexpr std::size_t kMaxArtworkEditTags = 200;

// 统计 UTF-8 码位数量（按字符而不是字节限制长度）。
std::size_t utf8_length(std::string_view text);

// 只检查 RFC3339 的固定形状，真正的日历/时区校验交给 PostgreSQL。
bool has_timestamp_shape(std::string_view value);

// 以下校验通过时返回 std::nullopt，否则返回给客户端看的错误描述。
std::optional<std::string>
validate_entity_ids(const std::vector<std::string> &entity_ids);
std::optional<std::string>
validate_image_ids(const std::vector<std::string> &image_ids);
std::optional<std::string> validate_title(const std::string &title);
std::optional<std::string>
validate_uuid_list(const std::vector<std::string> &ids, std::string_view field);
// 编辑作品时的标签 ID 列表：允许为空，限制数量、UUID 与重复项。
std::optional<std::string>
validate_tag_update_ids(const std::vector<std::string> &ids,
                        std::string_view field);
std::optional<std::string> validate_published_at(const std::string &value);

} // namespace tagimg
