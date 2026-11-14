#include "common/config.hpp"
#include "db/entity_repository.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

// 隐藏 userinfo 里的密码，避免冒烟测试输出把凭据带出去。
std::string mask_url(std::string_view url) {
  std::string masked(url);
  const auto scheme_end = masked.find("://");
  if (scheme_end == std::string::npos) {
    return masked;
  }
  const auto at = masked.find('@', scheme_end + 3);
  if (at == std::string::npos) {
    return masked;
  }
  const auto colon = masked.find(':', scheme_end + 3);
  if (colon == std::string::npos || colon > at) {
    return masked;
  }
  masked.replace(colon + 1, at - colon - 1, "***");
  return masked;
}

tagimg::EntityDbConfig to_entity_config(const tagimg::DatabaseConfig &db) {
  return tagimg::EntityDbConfig{
      .host = db.host,
      .port = db.port,
      .user = db.user,
      .password = db.password,
      .dbname = db.entity_dbname,
  };
}

} // namespace

int main(int argc, char **argv) {
  std::string config_path = "config.json";
  std::vector<std::string> entity_ids;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--config") {
      if (i + 1 >= argc) {
        std::cerr << "--config requires a path\n";
        return 2;
      }
      config_path = argv[++i];
    } else if (arg == "-h" || arg == "--help") {
      std::cout << "usage: " << argv[0]
                << " [--config PATH] ENTITY_ID [ENTITY_ID...]\n";
      return 0;
    } else {
      entity_ids.emplace_back(arg);
    }
  }

  if (entity_ids.empty()) {
    std::cerr << "usage: " << argv[0]
              << " [--config PATH] ENTITY_ID [ENTITY_ID...]\n";
    return 2;
  }

  const auto config = tagimg::load_config(config_path);
  if (!config) {
    std::cerr << "failed to load " << config_path << ": "
              << tagimg::to_string(config.error()) << '\n';
    return 2;
  }

  const auto repository =
      tagimg::EntityRepository::connect(to_entity_config(config->database));
  if (!repository) {
    std::cerr << "failed to connect to entity database: "
              << tagimg::to_string(repository.error()) << '\n';
    return 2;
  }

  auto output = nlohmann::json::array();
  bool failed = false;

  for (const auto &entity_id : entity_ids) {
    auto locations = (*repository)->find_webdav_locations(entity_id);
    nlohmann::json item;
    item["entity_id"] = entity_id;
    if (!locations) {
      failed = true;
      item["error"] = std::string(tagimg::to_string(locations.error()));
      output.push_back(std::move(item));
      continue;
    }

    auto entries = nlohmann::json::array();
    for (const auto &location : *locations) {
      nlohmann::json entry;
      entry["storage_id"] = location.storage_id;
      entry["storage_name"] = location.storage_name;
      entry["url"] = mask_url(location.url);
      entries.push_back(std::move(entry));
    }
    item["locations"] = std::move(entries);
    output.push_back(std::move(item));
  }

  std::cout << output.dump(2) << '\n';
  return failed ? 1 : 0;
}
