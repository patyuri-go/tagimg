#include "ai/clip_embedder.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

double norm(const std::vector<float> &values) {
  double sum = 0.0;
  for (const float value : values) {
    sum += static_cast<double>(value) * static_cast<double>(value);
  }
  return std::sqrt(sum);
}

nlohmann::json embedding_to_json(const tagimg::Embedding &embedding,
                                 std::size_t sample) {
  nlohmann::json item;
  item["dim"] = embedding.values.size();
  item["norm"] = norm(embedding.values);
  auto head = nlohmann::json::array();
  for (std::size_t index = 0;
       index < embedding.values.size() && index < sample; ++index) {
    head.push_back(embedding.values[index]);
  }
  item["sample"] = std::move(head);
  return item;
}

} // namespace

int main(int argc, char **argv) {
  const char *endpoint = std::getenv("TAGIMG_AI_ENDPOINT");
  tagimg::ClipEmbedder embedder(endpoint != nullptr ? endpoint
                                                    : "http://127.0.0.1:8001");
  auto output = nlohmann::json::array();
  bool failed = false;

  // 先跑文本，方便在只验证文本接口时不传图片。
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg.rfind("--text=", 0) != 0) {
      continue;
    }
    const std::string text = arg.substr(7);
    auto result = embedder.embed_text(text);
    nlohmann::json item;
    item["text"] = text;
    if (!result) {
      failed = true;
      item["error"] = std::string(tagimg::to_string(result.error()));
    } else {
      item.update(embedding_to_json(*result, 4));
    }
    output.push_back(std::move(item));
  }

  bool has_image = false;
  for (int index = 1; index < argc; ++index) {
    const std::string path = argv[index];
    if (path.rfind("--text=", 0) == 0) {
      continue;
    }
    has_image = true;
    auto result = embedder.embed_file(path);
    nlohmann::json item;
    item["path"] = path;
    if (!result) {
      failed = true;
      item["error"] = std::string(tagimg::to_string(result.error()));
    } else {
      item.update(embedding_to_json(*result, 4));
    }
    output.push_back(std::move(item));
  }

  if (!has_image && output.empty()) {
    std::cerr << "usage: " << argv[0]
              << " [--text=TEXT] IMAGE [IMAGE...]\n";
    return 2;
  }

  std::cout << output.dump(2) << '\n';
  return failed ? 1 : 0;
}
