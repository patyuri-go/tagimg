#include "common/validation.hpp"

#include <algorithm>

#include "common/uuid.hpp"

namespace tagimg {
namespace {

bool is_digit(char value) {
  return value >= '0' && value <= '9';
}

} // namespace

std::size_t utf8_length(std::string_view text) {
  std::size_t length = 0;
  for (const unsigned char byte : text) {
    if ((byte & 0xC0U) != 0x80U) {
      ++length;
    }
  }
  return length;
}

bool has_timestamp_shape(std::string_view value) {
  if (value.size() < 20) {
    return false;
  }
  for (const std::size_t index : {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15}) {
    if (!is_digit(value[index])) {
      return false;
    }
  }
  if (value[4] != '-' || value[7] != '-') {
    return false;
  }
  if (value[10] != 'T' && value[10] != 't') {
    return false;
  }
  return value[13] == ':' && value[16] == ':';
}

std::optional<std::string>
validate_entity_ids(const std::vector<std::string> &entity_ids) {
  if (entity_ids.empty() || entity_ids.size() > kMaxEntityIds) {
    return "entityIds must contain 1 to " + std::to_string(kMaxEntityIds) +
           " items";
  }
  for (const std::string &entity_id : entity_ids) {
    if (!is_uuid(entity_id)) {
      return "entityIds contains an invalid uuid";
    }
  }

  std::vector<std::string> sorted = entity_ids;
  std::sort(sorted.begin(), sorted.end());
  if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
    return "entityIds contains duplicate items";
  }
  return std::nullopt;
}

std::optional<std::string> validate_title(const std::string &title) {
  if (title.empty()) {
    return "title must not be empty";
  }
  if (utf8_length(title) > kMaxTitleLength) {
    return "title must be at most " + std::to_string(kMaxTitleLength) +
           " characters";
  }
  return std::nullopt;
}

std::optional<std::string>
validate_uuid_list(const std::vector<std::string> &ids,
                   std::string_view field) {
  for (const std::string &id : ids) {
    if (!is_uuid(id)) {
      return std::string(field) + " contains an invalid uuid";
    }
  }
  return std::nullopt;
}

std::optional<std::string> validate_published_at(const std::string &value) {
  if (!has_timestamp_shape(value)) {
    return "publishedAt must be an RFC3339 timestamp";
  }
  return std::nullopt;
}

} // namespace tagimg
