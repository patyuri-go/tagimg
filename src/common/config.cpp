#include "common/config.hpp"

#include <cmath>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>
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

std::string trim_ascii(std::string_view text) {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.remove_suffix(1);
  }
  return std::string(text);
}

// 标签路径由标准名用 :: 连接；名称本身不允许包含 ::，所以空段一定是配置
// 错误。完整路径可以只有一段（根本身）。
bool valid_tag_path(std::string_view path) {
  if (path.empty()) {
    return false;
  }
  std::size_t start = 0;
  for (;;) {
    const std::size_t end = path.find("::", start);
    const std::string_view segment =
        path.substr(start, end == std::string_view::npos
                               ? std::string_view::npos
                               : end - start);
    if (segment.empty()) {
      return false;
    }
    if (end == std::string_view::npos) {
      return true;
    }
    start = end + 2;
    if (start == path.size()) {
      return false;
    }
  }
}

// 解析 "2G" / "512MB" / 1024 这类容量写法。K/M/G/T 均按 1024 进制。
std::optional<std::uint64_t> parse_readable_size(std::string_view text) {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.remove_suffix(1);
  }
  if (text.empty()) {
    return std::nullopt;
  }

  std::uint64_t value = 0;
  std::size_t index = 0;
  bool has_digit = false;
  for (; index < text.size(); ++index) {
    const unsigned char ch = static_cast<unsigned char>(text[index]);
    if (std::isdigit(ch) == 0) {
      break;
    }
    has_digit = true;
    const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      return std::nullopt;
    }
    value = value * 10 + digit;
  }
  if (!has_digit) {
    return std::nullopt;
  }

  std::string suffix;
  for (; index < text.size(); ++index) {
    const unsigned char ch = static_cast<unsigned char>(text[index]);
    if (std::isspace(ch) != 0) {
      continue;
    }
    suffix.push_back(static_cast<char>(std::tolower(ch)));
  }

  std::uint64_t multiplier = 1;
  if (suffix.empty() || suffix == "b") {
    multiplier = 1;
  } else if (suffix == "k" || suffix == "kb" || suffix == "kib") {
    multiplier = 1024ULL;
  } else if (suffix == "m" || suffix == "mb" || suffix == "mib") {
    multiplier = 1024ULL * 1024;
  } else if (suffix == "g" || suffix == "gb" || suffix == "gib") {
    multiplier = 1024ULL * 1024 * 1024;
  } else if (suffix == "t" || suffix == "tb" || suffix == "tib") {
    multiplier = 1024ULL * 1024 * 1024 * 1024;
  } else {
    return std::nullopt;
  }

  if (value > std::numeric_limits<std::uint64_t>::max() / multiplier) {
    return std::nullopt;
  }
  return value * multiplier;
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

  // storage 段必填：没有存储源就无法读取图片内容。
  const auto storage = root.find("storage");
  if (storage == root.end() || !storage->is_object()) {
    return ConfigError::MissingField;
  }

  auto &source = config.storage;
  if (!read_required_string(*storage, "host", source.host, false)) {
    return ConfigError::MissingField;
  }

  const auto storage_port = storage->find("port");
  if (storage_port == storage->end()) {
    return ConfigError::MissingField;
  }
  if (!storage_port->is_number_integer()) {
    return ConfigError::InvalidField;
  }
  const auto storage_port_value = storage_port->get<long long>();
  if (storage_port_value < 1 || storage_port_value > 65535) {
    return ConfigError::InvalidField;
  }
  source.port = static_cast<std::uint16_t>(storage_port_value);

  const auto storage_scheme = storage->find("scheme");
  if (storage_scheme != storage->end()) {
    if (!storage_scheme->is_string()) {
      return ConfigError::InvalidField;
    }
    source.scheme = storage_scheme->get<std::string>();
    if (source.scheme != "http" && source.scheme != "https") {
      return ConfigError::InvalidField;
    }
  }

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

    // 图片流模式可选，缺省保持本机代理；值只能是 proxy 或 redirect。
    const auto image_mode = http->find("imageMode");
    if (image_mode != http->end()) {
      if (!image_mode->is_string()) {
        return ConfigError::InvalidField;
      }
      const std::string value = image_mode->get<std::string>();
      if (value == "proxy") {
        config.http.image_mode = ImageStreamMode::Proxy;
      } else if (value == "redirect") {
        config.http.image_mode = ImageStreamMode::Redirect;
      } else {
        return ConfigError::InvalidField;
      }
    }

    // 图片转码参数可选，缺省使用 HttpConfig 里的默认值。
    const auto read_http_integer =
        [&http](const char *key, long long minimum, long long maximum,
                std::uint32_t &output) -> std::optional<ConfigError> {
      const auto it = http->find(key);
      if (it == http->end()) {
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

    if (const auto error = read_http_integer(
            "imageQuality", 1, 100, config.http.image_quality)) {
      return *error;
    }
    if (const auto error =
            read_http_integer("imageSpeed", 0, 9, config.http.image_speed)) {
      return *error;
    }
    if (const auto error = read_http_integer(
            "imageMaxConcurrency", 1, 64, config.http.image_max_concurrency)) {
      return *error;
    }
    if (const auto error = read_http_integer(
            "imageTimeoutSeconds", 1, 300,
            config.http.image_timeout_seconds)) {
      return *error;
    }

    // 像素上限用 64 位存放，但 JSON 配置里限制在合理范围内。
    const auto max_pixels = http->find("imageMaxPixels");
    if (max_pixels != http->end()) {
      if (!max_pixels->is_number_integer()) {
        return ConfigError::InvalidField;
      }
      const auto value = max_pixels->get<long long>();
      if (value < 1 || value > 1'000'000'000LL) {
        return ConfigError::InvalidField;
      }
      config.http.image_max_pixels = static_cast<std::uint64_t>(value);
    }

    const auto read_optional_http_integer =
        [&http](const char *key, long long minimum, long long maximum,
                std::optional<std::uint32_t> &output)
        -> std::optional<ConfigError> {
      const auto it = http->find(key);
      if (it == http->end()) {
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

    if (const auto error = read_optional_http_integer(
            "imageAnimatedQuality", 1, 100,
            config.http.image_animated_quality)) {
      return *error;
    }
    if (const auto error = read_optional_http_integer(
            "imageAnimatedSpeed", 0, 9, config.http.image_animated_speed)) {
      return *error;
    }
    if (const auto error =
            read_http_integer("imageAnimatedMaxFrames", 1, 10000,
                              config.http.image_animated_max_frames)) {
      return *error;
    }

    const auto animated_pixels = http->find("imageAnimatedMaxTotalPixels");
    if (animated_pixels != http->end()) {
      if (!animated_pixels->is_number_integer()) {
        return ConfigError::InvalidField;
      }
      const auto value = animated_pixels->get<long long>();
      if (value < 1 || value > 10'000'000'000LL) {
        return ConfigError::InvalidField;
      }
      config.http.image_animated_max_total_pixels =
          static_cast<std::uint64_t>(value);
    }
  }

  // ai 段是必填项：wd14_threshold 没有可接受的隐式默认值。
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

    const auto read_boolean =
        [&ai](const char *key, bool &output) -> std::optional<ConfigError> {
      const auto it = ai->find(key);
      if (it == ai->end()) {
        return std::nullopt;
      }
      if (!it->is_boolean()) {
        return ConfigError::InvalidField;
      }
      output = it->get<bool>();
      return std::nullopt;
    };

    const auto threshold = ai->find("wd14_threshold");
    if (threshold == ai->end()) {
      return ConfigError::MissingField;
    }
    if (!threshold->is_number()) {
      return ConfigError::InvalidField;
    }
    const double threshold_value = threshold->get<double>();
    if (!std::isfinite(threshold_value) || threshold_value < 0.0 ||
        threshold_value > 1.0) {
      return ConfigError::InvalidField;
    }
    config.ai.wd14_threshold = static_cast<float>(threshold_value);

    if (const auto error = read_boolean("local", config.ai.local)) {
      return *error;
    }
    if (const auto error = read_string("host", config.ai.host)) {
      return *error;
    }
    if (const auto error = read_string("module", config.ai.module)) {
      return *error;
    }
    // 远程模式下不启动 Python，因此本地路径可以缺省甚至为空。
    if (config.ai.local) {
      if (const auto error = read_path("python", config.ai.python)) {
        return *error;
      }
      if (const auto error = read_path("python_path", config.ai.python_path)) {
        return *error;
      }
      if (const auto error = read_path("model_dir", config.ai.model_dir)) {
        return *error;
      }
    }

    std::uint32_t port_value = config.ai.port;
    if (const auto error = read_integer("port", 1, 65535, port_value)) {
      return *error;
    }
    config.ai.port = static_cast<std::uint16_t>(port_value);

    if (const auto error =
            read_integer("health_timeout_ms", 100, 10000,
                         config.ai.health_timeout_ms)) {
      return *error;
    }
    if (const auto error = read_integer("threads", 1, 64, config.ai.threads)) {
      return *error;
    }
    if (const auto error = read_integer("startup_timeout_seconds", 1, 600,
                                        config.ai.startup_timeout_seconds)) {
      return *error;
    }

    if (config.ai.local) {
      if (const auto error =
              read_path("clip_model_dir", config.ai.clip_model_dir)) {
        return *error;
      }
    }
    if (const auto error =
            read_integer("clip_threads", 1, 64, config.ai.clip_threads)) {
      return *error;
    }
    if (const auto error = read_boolean("clip_preload", config.ai.clip_preload)) {
      return *error;
    }
    if (const auto error = read_boolean("clip_strict", config.ai.clip_strict)) {
      return *error;
    }
    config.ai.clip_strict_configured = ai->contains("clip_strict");
    if (const auto error = read_integer("clip_timeout_seconds", 1, 600,
                                        config.ai.clip_timeout_seconds)) {
      return *error;
    }
    if (const auto error = read_integer("clip_startup_timeout_seconds", 1, 1800,
                                        config.ai.clip_startup_timeout_seconds)) {
      return *error;
    }
  } else {
    return ConfigError::MissingField;
  }

  // cache 段可省略；省略时缓存层禁用。写了就逐项校验。
  const auto cache = root.find("cache");
  if (cache != root.end()) {
    if (!cache->is_object()) {
      return ConfigError::InvalidField;
    }
    config.cache.enabled = true;

    const auto enabled = cache->find("enabled");
    if (enabled != cache->end()) {
      if (!enabled->is_boolean()) {
        return ConfigError::InvalidField;
      }
      config.cache.enabled = enabled->get<bool>();
    }

    const auto directory = cache->find("directory");
    if (directory != cache->end()) {
      if (!directory->is_string()) {
        return ConfigError::InvalidField;
      }
      const std::string value = directory->get<std::string>();
      if (value.empty()) {
        return ConfigError::MissingField;
      }
      config.cache.directory = value;
    }

    const auto max_size = cache->find("maxSize");
    if (max_size != cache->end()) {
      if (max_size->is_string()) {
        const auto parsed =
            parse_readable_size(max_size->get<std::string>());
        if (!parsed) {
          return ConfigError::InvalidField;
        }
        config.cache.max_bytes = *parsed;
      } else if (max_size->is_number_integer()) {
        const auto value = max_size->get<long long>();
        if (value < 0) {
          return ConfigError::InvalidField;
        }
        config.cache.max_bytes = static_cast<std::uint64_t>(value);
      } else {
        return ConfigError::InvalidField;
      }
    }

    const auto reconcile = cache->find("reconcileOnOpen");
    if (reconcile != cache->end()) {
      if (!reconcile->is_boolean()) {
        return ConfigError::InvalidField;
      }
      config.cache.reconcile_on_open = reconcile->get<bool>();
    }

    // 与 AI 路径一致：相对路径按配置文件所在目录解析。
    if (config.cache.directory.is_relative()) {
      auto base = path.parent_path();
      if (base.empty()) {
        base = std::filesystem::path(".");
      }
      config.cache.directory = base / config.cache.directory;
    }
  }

  // search 段可省略；省略或空数组表示不启用默认排除。
  const auto search = root.find("search");
  if (search != root.end()) {
    if (!search->is_object()) {
      return ConfigError::InvalidField;
    }
    const auto paths = search->find("defaultExcludedTagPaths");
    if (paths != search->end()) {
      if (!paths->is_array()) {
        return ConfigError::InvalidField;
      }
      for (const auto &entry : *paths) {
        if (!entry.is_string()) {
          return ConfigError::InvalidField;
        }
        std::string tag_path = trim_ascii(entry.get<std::string>());
        if (!valid_tag_path(tag_path)) {
          return ConfigError::InvalidField;
        }
        config.search.default_excluded_tag_paths.push_back(
            std::move(tag_path));
      }
    }
  }

  return config;
}

} // namespace tagimg
