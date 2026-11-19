#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "ai/clip_embedder.hpp"
#include "ai/health.hpp"
#include "ai/tagger.hpp"
#include "common/config.hpp"
#include "db/entity_repository.hpp"
#include "db/tagimg_repository.hpp"
#include "job/add_job.hpp"
#include "job/job.hpp"

namespace {

int print_usage(const char *program) {
  std::cerr << "usage: " << program
            << " [--config PATH] [--endpoint URL] [--work-dir DIR]"
               " [--title TITLE] ENTITY_ID\n";
  return 2;
}

} // namespace

int main(int argc, char **argv) {
  std::filesystem::path config_path = "config.json";
  std::string endpoint = "http://127.0.0.1:8001";
  std::filesystem::path work_dir = "tmp";
  std::string title = "job-smoke-test";
  std::string entity_id;

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
    } else if (argument == "--title") {
      title = next_value();
    } else if (argument.rfind("--", 0) == 0) {
      return print_usage(argv[0]);
    } else {
      entity_id = argument;
    }
  }

  if (entity_id.empty()) {
    return print_usage(argv[0]);
  }

  const auto config = tagimg::load_config(config_path);
  if (!config) {
    std::cerr << "config error: " << tagimg::to_string(config.error()) << '\n';
    return 2;
  }

  tagimg::EntityDbConfig entity_db_config;
  entity_db_config.host = config->database.host;
  entity_db_config.port = config->database.port;
  entity_db_config.user = config->database.user;
  entity_db_config.password = config->database.password;
  entity_db_config.dbname = config->database.entity_dbname;

  auto repository = tagimg::EntityRepository::connect(entity_db_config);
  if (!repository) {
    std::cerr << "connect error: " << tagimg::to_string(repository.error())
              << '\n';
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
  tagimg::JobContext context{std::cref(**repository),
                             std::cref(**tagimg_repository), std::ref(tagger),
                             std::ref(embedder), std::cref(ai_health),
                             work_dir};

  tagimg::JobWorker worker(context);

  tagimg::AddRequest request;
  request.entity_ids.push_back(entity_id);
  request.title = title;

  const auto add_start = std::chrono::steady_clock::now();
  const tagimg::JobId add_id =
      worker.submit(std::make_unique<tagimg::AddJob>(std::move(request)), true);
  if (add_id == 0) {
    std::cerr << "submit error: worker is stopped\n";
    return 2;
  }

  auto add_outcome = worker.wait(add_id);
  const auto add_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - add_start)
          .count();

  nlohmann::json output;
  output["add_job_id"] = add_id;
  output["add_elapsed_ms"] = add_elapsed;
  output["add_ok"] = static_cast<bool>(add_outcome);

  if (add_outcome) {
    const auto *result = std::get_if<tagimg::AddResult>(&*add_outcome);
    if (result != nullptr) {
      output["artwork_id"] = result->artwork_id;
      output["image_ids"] = result->image_ids;
      output["failure_count"] = result->failures.size();
    }
  } else {
    output["add_error"] = std::string(tagimg::to_string(add_outcome.error()));
  }

  std::cout << output.dump(2) << '\n';
  return add_outcome ? 0 : 1;
}
