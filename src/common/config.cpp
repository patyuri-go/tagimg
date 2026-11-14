#include "common/config.hpp"

#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>

#include <nlohmann/json.hpp>

namespace tagimg {
namespace {

using Json = nlohmann::json;

bool read_required_string(const Json &object, const char *key,
                          std::string &output, bool allow_empty) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_string()) {
    return false;
  }
  output = it->get<std::string>();
  return allow_empty || !output.empty();
}

} // namespace

std::string_view to_string(ConfigError error) noexcept {
  switch (error) {
  case ConfigError::FileNotFound:
    return "FileNotFound";
  case ConfigError::ReadError:
    return "ReadError";
  case ConfigError::ParseError:
    return "ParseError";
  case ConfigError::MissingField:
    return "MissingField";
  case ConfigError::InvalidField:
    return "InvalidField";
  }
  return "Unknown";
}

Result<AppConfig, ConfigError> load_config(const std::filesystem::path &path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    return error ? ConfigError::ReadError : ConfigError::FileNotFound;
  }
  if (!std::filesystem::is_regular_file(path, error) || error) {
    return ConfigError::ReadError;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return ConfigError::ReadError;
  }
  const std::string text((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  if (input.bad()) {
    return ConfigError::ReadError;
  }

  Json root;
  try {
    root = Json::parse(text);
  } catch (const Json::exception &) {
    return ConfigError::ParseError;
  }

  if (!root.is_object()) {
    return ConfigError::ParseError;
  }
  const auto database = root.find("database");
  if (database == root.end() || !database->is_object()) {
    return ConfigError::MissingField;
  }

  AppConfig config;
  auto &db = config.database;

  if (!read_required_string(*database, "host", db.host, false) ||
      !read_required_string(*database, "user", db.user, false) ||
      !read_required_string(*database, "password", db.password, true) ||
      !read_required_string(*database, "image_dbname", db.image_dbname,
                            false) ||
      !read_required_string(*database, "entity_dbname", db.entity_dbname,
                            false)) {
    return ConfigError::MissingField;
  }

  const auto port = database->find("port");
  if (port == database->end()) {
    return ConfigError::MissingField;
  }
  if (!port->is_number_integer()) {
    return ConfigError::InvalidField;
  }
  const auto port_value = port->get<long long>();
  if (port_value < 1 || port_value > 65535) {
    return ConfigError::InvalidField;
  }
  db.port = static_cast<std::uint16_t>(port_value);

  // http 段是可选配置；写了就必须完整，避免监听地址悄悄退回默认值。
  const auto http = root.find("http");
  if (http != root.end()) {
    if (!http->is_object()) {
      return ConfigError::InvalidField;
    }
    if (!read_required_string(*http, "host", config.http.host, false)) {
      return ConfigError::MissingField;
    }
    const auto http_port = http->find("port");
    if (http_port == http->end()) {
      return ConfigError::MissingField;
    }
    if (!http_port->is_number_integer()) {
      return ConfigError::InvalidField;
    }
    const auto http_port_value = http_port->get<long long>();
    if (http_port_value < 1 || http_port_value > 65535) {
      return ConfigError::InvalidField;
    }
    config.http.port = static_cast<std::uint16_t>(http_port_value);
  }

  // ai 段同样是可选项；写了的字段会做类型校验，没写的用默认值。
  const auto ai = root.find("ai");
  if (ai != root.end()) {
    if (!ai->is_object()) {
      return ConfigError::InvalidField;
    }

    const auto read_string =
        [&ai](const char *key,
              std::string &output) -> std::optional<ConfigError> {
      const auto it = ai->find(key);
      if (it == ai->end()) {
        return std::nullopt;
      }
      if (!it->is_string()) {
        return ConfigError::InvalidField;
      }
      std::string value = it->get<std::string>();
      if (value.empty()) {
        return ConfigError::MissingField;
      }
      output = std::move(value);
      return std::nullopt;
    };

    const auto read_path =
        [&ai](const char *key,
              std::filesystem::path &output) -> std::optional<ConfigError> {
      const auto it = ai->find(key);
      if (it == ai->end()) {
        return std::nullopt;
      }
      if (!it->is_string()) {
        return ConfigError::InvalidField;
      }
      std::filesystem::path value = it->get<std::string>();
      if (value.empty()) {
        return ConfigError::MissingField;
      }
      output = std::move(value);
      return std::nullopt;
    };

    const auto read_integer =
        [&ai](const char *key, long long minimum, long long maximum,
              std::uint32_t &output) -> std::optional<ConfigError> {
      const auto it = ai->find(key);
      if (it == ai->end()) {
        return std::nullopt;
      }
      if (!it->is_number_integer()) {
        return ConfigError::InvalidField;
      }
      const auto value = it->get<long long>();
      if (value < minimum || value > maximum) {
        return ConfigError::InvalidField;
      }
      output = static_cast<std::uint32_t>(value);
      return std::nullopt;
    };

    if (const auto error = read_string("host", config.ai.host)) {
      return *error;
    }
    if (const auto error = read_string("module", config.ai.module)) {
      return *error;
    }
    if (const auto error = read_path("python", config.ai.python)) {
      return *error;
    }
    if (const auto error = read_path("python_path", config.ai.python_path)) {
      return *error;
    }
    if (const auto error = read_path("model_dir", config.ai.model_dir)) {
      return *error;
    }

    std::uint32_t port_value = config.ai.port;
    if (const auto error = read_integer("port", 1, 65535, port_value)) {
      return *error;
    }
    config.ai.port = static_cast<std::uint16_t>(port_value);

    if (const auto error = read_integer("threads", 1, 64, config.ai.threads)) {
      return *error;
    }
    if (const auto error = read_integer("startup_timeout_seconds", 1, 600,
                                        config.ai.startup_timeout_seconds)) {
      return *error;
    }
  }

  return config;
}

} // namespace tagimg
