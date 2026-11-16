#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/config.hpp"
#include "db/tagimg_repository.hpp"

namespace {

constexpr std::size_t kDim = 768;

// 造一个确定性的单位向量，便于重复核对写入和读回是否一致。
std::vector<float> make_vector(std::size_t seed) {
  std::vector<float> values(kDim, 0.0F);
  for (std::size_t index = 0; index < kDim; ++index) {
    values[index] = static_cast<float>((index * 31 + seed * 17) % 200 - 100);
  }
  double squared = 0.0;
  for (float value : values) {
    squared += static_cast<double>(value) * static_cast<double>(value);
  }
  const double length = squared > 0.0 ? std::sqrt(squared) : 1.0;
  for (float &value : values) {
    value = static_cast<float>(static_cast<double>(value) / length);
  }
  return values;
}

double cosine(const std::vector<float> &left, const std::vector<float> &right) {
  double dot = 0.0;
  for (std::size_t index = 0; index < left.size() && index < right.size();
       ++index) {
    dot += static_cast<double>(left[index]) * static_cast<double>(right[index]);
  }
  return dot;
}

} // namespace

int main(int argc, char **argv) {
  std::filesystem::path config_path = "config.json";
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--config" && index + 1 < argc) {
      config_path = argv[++index];
    } else {
      std::cerr << "usage: " << argv[0] << " [--config PATH]\n";
      return 2;
    }
  }

  const auto config = tagimg::load_config(config_path);
  if (!config) {
    std::cerr << "config error: " << tagimg::to_string(config.error()) << '\n';
    return 2;
  }

  tagimg::TagimgDbConfig db;
  db.host = config->database.host;
  db.port = config->database.port;
  db.user = config->database.user;
  db.password = config->database.password;
  db.dbname = config->database.image_dbname;

  auto repository = tagimg::TagimgRepository::connect(db);
  if (!repository) {
    std::cerr << "connect error: " << tagimg::to_string(repository.error())
              << '\n';
    return 2;
  }

  nlohmann::json output;
  bool failed = false;

  // 1. 列出缺少向量的图片，确认查询可用。
  auto missing = (*repository)->list_images_missing_embedding(5, 0);
  if (!missing) {
    std::cerr << "list error: " << tagimg::to_string(missing.error()) << '\n';
    return 1;
  }
  output["missing_count"] = missing->size();
  auto missing_ids = nlohmann::json::array();
  for (const auto &row : *missing) {
    missing_ids.push_back(
        {{"image_id", row.image_id},
         {"entity_id", row.entity_id ? nlohmann::json(*row.entity_id)
                                     : nlohmann::json(nullptr)}});
  }
  output["missing"] = std::move(missing_ids);

  if (missing->empty()) {
    output["note"] = "no image without embedding; skip write checks";
    std::cout << output.dump(2) << '\n';
    return 0;
  }

  // 2. 对第一张缺向量的图片写入，再读回比对。
  const std::string image_id = missing->front().image_id;
  const std::vector<float> values = make_vector(7);
  auto written = (*repository)->save_clip_embeddings({{image_id, values}});
  if (!written) {
    std::cerr << "save error: " << tagimg::to_string(written.error()) << '\n';
    return 1;
  }
  output["written"] = *written;

  auto found = (*repository)->find_clip_embedding(image_id);
  if (!found) {
    std::cerr << "find error: " << tagimg::to_string(found.error()) << '\n';
    return 1;
  }
  output["image_id"] = image_id;
  output["found"] = found->has_value();
  if (*found) {
    output["found_dim"] = (*found)->size();
    output["cosine_with_written"] = cosine(values, **found);
    if ((*found)->size() != kDim) {
      failed = true;
    }
  } else {
    failed = true;
  }

  // 3. 再列出缺向量，刚写入的那张不应再出现。
  auto after = (*repository)->list_images_missing_embedding(5, 0);
  if (!after) {
    std::cerr << "list error: " << tagimg::to_string(after.error()) << '\n';
    return 1;
  }
  bool still_missing = false;
  for (const auto &row : *after) {
    if (row.image_id == image_id) {
      still_missing = true;
    }
  }
  output["still_missing"] = still_missing;
  if (still_missing) {
    failed = true;
  }

  // 4. 维度不符必须被拒绝。
  auto bad = (*repository)->save_clip_embeddings({{image_id, {1.0F, 2.0F}}});
  output["reject_wrong_dim"] = !bad;
  if (bad) {
    failed = true;
  }

  std::cout << output.dump(2) << '\n';
  return failed ? 1 : 0;
}
