#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "ai/clip_embedder.hpp"
#include "ai/health.hpp"
#include "ai/tagger.hpp"
#include "common/config.hpp"
#include "db/tagimg_repository.hpp"
#include "job/add_job.hpp"
#include "job/job.hpp"
#include "job/scan_job.hpp"

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

nlohmann::json optional_to_json(const std::optional<std::string> &value) {
  if (!value) {
    return nullptr;
  }
  return *value;
}

nlohmann::json scan_to_json(const tagimg::ScanResult &scan) {
  nlohmann::json item;
  item["entity_id"] = scan.entity_id;
  item["bytes"] = scan.bytes;
  item["general"] = tags_to_json(scan.tags.general);
  item["character"] = tags_to_json(scan.tags.character);
  item["rating"] = tags_to_json(scan.tags.rating);
  // 向量本身太大，冒烟只报告维度与模长，确认归一化生效。
  item["embedding_dim"] = scan.embedding.size();
  double squared = 0.0;
  for (float value : scan.embedding) {
    squared += static_cast<double>(value) * static_cast<double>(value);
  }
  item["embedding_norm"] = std::sqrt(squared);
  return item;
}

int print_usage(const char *program) {
  std::cerr << "usage: " << program
            << " [--config PATH] [--endpoint URL] [--work-dir DIR] ENTITY_ID..."
               "\n"
               "       "
            << program
            << " --add --title TITLE [--description TEXT] [--published-at TIME]"
               " [--tag-id UUID]... [--artist-id UUID]... ENTITY_ID...\n";
  return 2;
}

} // namespace

int main(int argc, char **argv) {
  std::filesystem::path config_path = "config.json";
  std::string endpoint = "http://127.0.0.1:8001";
  std::filesystem::path work_dir = "tmp";
  bool add_mode = false;
  tagimg::AddRequest request;
  std::vector<std::string> entity_ids;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto next_value = [&]() -> std::string {
      if (index + 1 >= argc) {
        return {};
      }
      return argv[++index];
    };

    if (argument == "--config") {
      config_path = next_value();
    } else if (argument == "--endpoint") {
      endpoint = next_value();
    } else if (argument == "--work-dir") {
      work_dir = next_value();
    } else if (argument == "--add") {
      add_mode = true;
    } else if (argument == "--title") {
      request.title = next_value();
    } else if (argument == "--description") {
      request.description = next_value();
    } else if (argument == "--published-at") {
      request.published_at = next_value();
    } else if (argument == "--tag-id") {
      request.tag_ids.push_back(next_value());
    } else if (argument == "--artist-id") {
      request.artist_ids.push_back(next_value());
    } else if (argument.rfind("--", 0) == 0) {
      return print_usage(argv[0]);
    } else {
      entity_ids.push_back(argument);
    }
  }

  if (entity_ids.empty()) {
    return print_usage(argv[0]);
  }

  const auto config = tagimg::load_config(config_path);
  if (!config) {
    std::cerr << "config error: " << tagimg::to_string(config.error()) << '\n';
    return 2;
  }

  tagimg::TagimgDbConfig tagimg_db_config;
  tagimg_db_config.host = config->database.host;
  tagimg_db_config.port = config->database.port;
  tagimg_db_config.user = config->database.user;
  tagimg_db_config.password = config->database.password;
  tagimg_db_config.dbname = config->database.image_dbname;

  auto tagimg_repository = tagimg::TagimgRepository::connect(tagimg_db_config);
  if (!tagimg_repository) {
    std::cerr << "tagimg connect error: "
              << tagimg::to_string(tagimg_repository.error()) << '\n';
    return 2;
  }

  tagimg::WdTagger tagger(endpoint, config->ai.wd14_threshold);
  tagimg::ClipEmbedder embedder(endpoint);
  tagimg::AiHealthClient ai_health(endpoint);
  tagimg::JobContext context{config->storage,
                             std::cref(**tagimg_repository), std::ref(tagger),
                             std::ref(embedder), std::cref(ai_health),
                             work_dir};

  nlohmann::json output;
  bool failed = false;

  if (add_mode) {
    request.entity_ids = entity_ids;

    tagimg::JobWorker worker(context);

    const tagimg::JobId job_id = worker.submit(
        std::make_unique<tagimg::AddJob>(std::move(request)), true);
    if (job_id == 0) {
      std::cerr << "submit error: worker is stopped\n";
      return 2;
    }

    auto outcome = worker.wait(job_id);
    if (!outcome) {
      std::cerr << "add error: " << tagimg::to_string(outcome.error()) << '\n';
      return 1;
    }

    const auto *result = std::get_if<tagimg::AddResult>(&*outcome);
    if (result == nullptr) {
      std::cerr << "add error: unexpected job outcome\n";
      return 1;
    }

    auto scans = nlohmann::json::array();
    for (const auto &scan : result->scans) {
      scans.push_back(scan_to_json(scan));
    }
    auto failures = nlohmann::json::array();
    for (const auto &failure : result->failures) {
      failed = true;
      failures.push_back(
          {{"entity_id", failure.entity_id},
           {"error", std::string(tagimg::to_string(failure.error))}});
    }

    output["title"] = result->request.title;
    output["description"] = optional_to_json(result->request.description);
    output["published_at"] = optional_to_json(result->request.published_at);
    output["tag_ids"] = result->request.tag_ids;
    output["artist_ids"] = result->request.artist_ids;
    output["artwork_id"] = result->artwork_id;
    output["image_ids"] = result->image_ids;
    output["scans"] = std::move(scans);
    output["failures"] = std::move(failures);
  } else {
    output = nlohmann::json::array();
    tagimg::JobWorker scan_worker(context);

    for (const std::string &entity_id : entity_ids) {
      const tagimg::JobId job_id = scan_worker.submit(
          std::make_unique<tagimg::ScanJob>(entity_id), true);
      if (job_id == 0) {
        std::cerr << "submit error: worker is stopped\n";
        return 2;
      }

      auto outcome = scan_worker.wait(job_id);
      if (!outcome) {
        failed = true;
        output.push_back(
            {{"entity_id", entity_id},
             {"error", std::string(tagimg::to_string(outcome.error()))}});
        continue;
      }

      const auto *scan = std::get_if<tagimg::ScanResult>(&*outcome);
      if (scan == nullptr) {
        failed = true;
        output.push_back({{"entity_id", entity_id},
                          {"error", std::string(tagimg::to_string(
                                        tagimg::JobError::InternalError))}});
        continue;
      }
      output.push_back(scan_to_json(*scan));
    }
  }

  std::cout << output.dump(2) << '\n';
  return failed ? 1 : 0;
}
