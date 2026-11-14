#include "ai/tagger.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

nlohmann::json tags_to_json(const std::vector<tagimg::TagScore> &tags) {
  auto array = nlohmann::json::array();
  for (const auto &tag : tags) {
    nlohmann::json item;
    item["name"] = tag.name;
    item["score"] = tag.score;
    array.push_back(std::move(item));
  }
  return array;
}

nlohmann::json result_to_json(const tagimg::TaggingResult &result) {
  nlohmann::json item;
  item["general"] = tags_to_json(result.general);
  item["character"] = tags_to_json(result.character);
  item["rating"] = tags_to_json(result.rating);
  return item;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << " IMAGE [IMAGE...]\n";
    return 2;
  }

  const char *endpoint = std::getenv("TAGIMG_AI_ENDPOINT");
  tagimg::WdTagger tagger(endpoint != nullptr ? endpoint
                                              : "http://127.0.0.1:8001");
  auto output = nlohmann::json::array();
  bool failed = false;

  for (int index = 1; index < argc; ++index) {
    const std::string path = argv[index];
    auto result = tagger.tag_file(path);
    if (!result) {
      failed = true;
      nlohmann::json item;
      item["path"] = path;
      item["error"] = std::string(tagimg::to_string(result.error()));
      output.push_back(std::move(item));
      continue;
    }

    nlohmann::json item = result_to_json(*result);
    item["path"] = path;
    output.push_back(std::move(item));
  }

  std::cout << output.dump(2) << '\n';
  return failed ? 1 : 0;
}
