// 图片缓存层 smoke：不依赖数据库、sidecar 或网络。
//
// 覆盖：未命中、写入/提交/读取、未提交清理、优先级淘汰、LRU 刷新、
// 占用中跳过、超上限放弃、自愈、启动对账、并发写同一 key。
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "cache/image_cache_store.hpp"
#include "common/image_size.hpp"

namespace {

using tagimg::ImageCacheCommit;
using tagimg::ImageCacheConfig;
using tagimg::ImageCacheKey;
using tagimg::ImageCacheStore;
using tagimg::ImageSizePreset;

int g_failures = 0;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": "              \
                << #condition << '\n';                                         \
      ++g_failures;                                                            \
      return false;                                                            \
    }                                                                          \
  } while (false)

std::filesystem::path make_root(const std::string &name) {
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("tagimg_cache_smoke_" + name + "_" + std::to_string(::getpid()) +
          "_" + std::to_string(stamp));
}

ImageCacheConfig make_config(const std::filesystem::path &root,
                             std::uint64_t max_bytes) {
  ImageCacheConfig config;
  config.enabled = true;
  config.directory = root;
  config.max_bytes = max_bytes;
  config.reconcile_on_open = true;
  return config;
}

ImageCacheKey make_key(std::string image_id, ImageSizePreset preset,
                       std::string extension = "bin",
                       std::string content_type = "") {
  ImageCacheKey key;
  key.image_id = std::move(image_id);
  key.params.preset = preset;
  key.params.quality = 65;
  key.params.speed = 6;
  key.params.animated_quality = 20;
  key.params.animated_speed = 9;
  key.params.animated_max_frames = 300;
  key.params.animated_max_total_pixels = 300000000;
  key.params.max_output_bytes = 52428800;
  key.params.extension = std::move(extension);
  key.params.content_type = std::move(content_type);
  return key;
}

bool write_payload(ImageCacheStore &store, const ImageCacheKey &key,
                   const std::string &payload, ImageCacheCommit *outcome) {
  auto created = store.create(key);
  if (created.has_error()) {
    std::cerr << "create failed: " << tagimg::to_string(created.error())
              << '\n';
    return false;
  }
  auto object = std::move(created.value());
  object.write().write(payload.data(),
                       static_cast<std::streamsize>(payload.size()));
  if (!object.write()) {
    std::cerr << "write stream failed\n";
    return false;
  }
  auto committed = object.commit();
  if (committed.has_error()) {
    std::cerr << "commit failed: " << tagimg::to_string(committed.error())
              << '\n';
    return false;
  }
  if (outcome != nullptr) {
    *outcome = committed.value();
  }
  return true;
}

bool exists(ImageCacheStore &store, const ImageCacheKey &key) {
  auto found = store.find(key);
  return found.has_value() && found->has_value();
}

bool read_all(tagimg::StoredImageCacheObject &object, std::string &output) {
  output.assign(std::istreambuf_iterator<char>(object.read()),
                std::istreambuf_iterator<char>());
  return object.read().good() || object.read().eof();
}

std::size_t count_part_files(const std::filesystem::path &root) {
  std::size_t count = 0;
  std::error_code error;
  for (auto it = std::filesystem::recursive_directory_iterator(root, error);
       !error && it != std::filesystem::recursive_directory_iterator();
       it.increment(error)) {
    if (it->is_regular_file() && it->path().extension() == ".part") {
      ++count;
    }
  }
  return count;
}

bool test_miss() {
  const auto root = make_root("miss");
  {
    auto opened =
        ImageCacheStore::open(make_config(root, 0), root.parent_path());
    CHECK(opened.has_value());
    auto store = std::move(opened.value());
    auto found = store.find(make_key("img-miss", ImageSizePreset::Thumb));
    CHECK(found.has_value());
    CHECK(!found->has_value());
  }
  std::filesystem::remove_all(root);
  return true;
}

bool test_write_commit_read() {
  const auto root = make_root("write");
  {
    auto opened =
        ImageCacheStore::open(make_config(root, 0), root.parent_path());
    CHECK(opened.has_value());
    auto store = std::move(opened.value());

    const std::string payload = "hello cache";
    const auto key =
        make_key("img-write", ImageSizePreset::Small, "avif", "image/avif");
    ImageCacheCommit outcome = ImageCacheCommit::SkippedOverLimit;
    CHECK(write_payload(store, key, payload, &outcome));
    CHECK(outcome == ImageCacheCommit::Committed);

    auto found = store.find(key);
    CHECK(found.has_value());
    CHECK(found->has_value());
    auto &object = **found;
    CHECK(object.readable());
    CHECK(!object.writable());
    CHECK(object.content_type() == "image/avif");
    CHECK(object.size() == payload.size());
    std::string read_back;
    CHECK(read_all(object, read_back));
    CHECK(read_back == payload);
    CHECK(std::filesystem::exists(object.file_path()));
  }
  std::filesystem::remove_all(root);
  return true;
}

bool test_abort_cleanup() {
  const auto root = make_root("abort");
  {
    auto opened =
        ImageCacheStore::open(make_config(root, 0), root.parent_path());
    CHECK(opened.has_value());
    auto store = std::move(opened.value());
    const auto key = make_key("img-abort", ImageSizePreset::Thumb);
    {
      auto created = store.create(key);
      CHECK(created.has_value());
      auto object = std::move(created.value());
      object.write().write("partial", 7);
      CHECK(object.writable());
    }
    CHECK(!exists(store, key));
    CHECK(count_part_files(root) == 0);
  }
  std::filesystem::remove_all(root);
  return true;
}

bool test_eviction_priority() {
  const auto root = make_root("priority");
  {
    auto opened =
        ImageCacheStore::open(make_config(root, 30), root.parent_path());
    CHECK(opened.has_value());
    auto store = std::move(opened.value());

    const auto mini = make_key("img-mini", ImageSizePreset::Mini);
    const auto thumb = make_key("img-thumb", ImageSizePreset::Thumb);
    const auto regular = make_key("img-regular", ImageSizePreset::Regular);
    const auto original = make_key("img-original", ImageSizePreset::Original);
    CHECK(write_payload(store, mini, "0123456789", nullptr));
    CHECK(write_payload(store, thumb, "0123456789", nullptr));
    CHECK(write_payload(store, regular, "0123456789", nullptr));
    CHECK(exists(store, mini));
    CHECK(exists(store, thumb));
    CHECK(exists(store, regular));

    CHECK(write_payload(store, original, "0123456789", nullptr));
    CHECK(exists(store, mini));
    CHECK(exists(store, thumb));
    CHECK(!exists(store, regular));
    CHECK(exists(store, original));

    const auto small = make_key("img-small", ImageSizePreset::Small);
    CHECK(write_payload(store, small, "0123456789", nullptr));
    CHECK(!exists(store, original));
    CHECK(exists(store, small));
    CHECK(exists(store, thumb));
    CHECK(exists(store, mini));

    const auto regular2 = make_key("img-regular-2", ImageSizePreset::Regular);
    CHECK(write_payload(store, regular2, "0123456789", nullptr));
    CHECK(!exists(store, small));
    CHECK(exists(store, regular2));
    CHECK(exists(store, thumb));
    CHECK(exists(store, mini));
  }
  std::filesystem::remove_all(root);
  return true;
}

bool test_last_hit_lru() {
  const auto root = make_root("lru");
  {
    auto opened =
        ImageCacheStore::open(make_config(root, 20), root.parent_path());
    CHECK(opened.has_value());
    auto store = std::move(opened.value());

    const auto a = make_key("img-a", ImageSizePreset::Thumb);
    const auto b = make_key("img-b", ImageSizePreset::Thumb);
    const auto c = make_key("img-c", ImageSizePreset::Thumb);
    CHECK(write_payload(store, a, "AAAAAAAAAA", nullptr));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(write_payload(store, b, "BBBBBBBBBB", nullptr));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(exists(store, a));

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(write_payload(store, c, "CCCCCCCCCC", nullptr));
    CHECK(exists(store, a));
    CHECK(!exists(store, b));
    CHECK(exists(store, c));
  }
  std::filesystem::remove_all(root);
  return true;
}

bool test_skip_in_use() {
  const auto root = make_root("inuse");
  {
    auto opened =
        ImageCacheStore::open(make_config(root, 20), root.parent_path());
    CHECK(opened.has_value());
    auto store = std::move(opened.value());

    const auto held = make_key("img-held", ImageSizePreset::Original);
    const auto other = make_key("img-other", ImageSizePreset::Thumb);
    const auto fresh = make_key("img-fresh", ImageSizePreset::Small);
    CHECK(write_payload(store, held, "0123456789", nullptr));
    CHECK(write_payload(store, other, "0123456789", nullptr));

    auto held_object = store.find(held);
    CHECK(held_object.has_value());
    CHECK(held_object->has_value());

    CHECK(write_payload(store, fresh, "0123456789", nullptr));
    CHECK(exists(store, held));
    CHECK(!exists(store, other));
    CHECK(exists(store, fresh));
  }
  std::filesystem::remove_all(root);
  return true;
}

bool test_oversize_skip() {
  const auto root = make_root("oversize");
  {
    auto opened =
        ImageCacheStore::open(make_config(root, 64), root.parent_path());
    CHECK(opened.has_value());
    auto store = std::move(opened.value());
    const auto key = make_key("img-big", ImageSizePreset::Regular);
    ImageCacheCommit outcome = ImageCacheCommit::Committed;
    CHECK(write_payload(store, key, std::string(65, 'x'), &outcome));
    CHECK(outcome == ImageCacheCommit::SkippedOverLimit);
    CHECK(!exists(store, key));
    CHECK(count_part_files(root) == 0);
  }
  std::filesystem::remove_all(root);
  return true;
}

bool test_self_heal() {
  const auto root = make_root("heal");
  {
    auto opened =
        ImageCacheStore::open(make_config(root, 0), root.parent_path());
    CHECK(opened.has_value());
    auto store = std::move(opened.value());
    const auto key = make_key("img-heal", ImageSizePreset::Thumb);
    CHECK(write_payload(store, key, "0123456789", nullptr));

    std::filesystem::path path;
    {
      auto found = store.find(key);
      CHECK(found.has_value());
      CHECK(found->has_value());
      path = (*found)->file_path();
    }
    CHECK(std::filesystem::exists(path));

    std::filesystem::remove(path);
    CHECK(!exists(store, key));

    CHECK(write_payload(store, key, "0123456789", nullptr));
    {
      auto found = store.find(key);
      CHECK(found.has_value());
      CHECK(found->has_value());
      path = (*found)->file_path();
    }
    {
      std::ofstream truncate(path, std::ios::binary | std::ios::trunc);
      truncate.write("abc", 3);
    }
    CHECK(!exists(store, key));
    CHECK(write_payload(store, key, "0123456789", nullptr));
    CHECK(exists(store, key));
  }
  std::filesystem::remove_all(root);
  return true;
}

bool test_reconcile() {
  const auto root = make_root("reconcile");
  const auto key = make_key("img-reconcile", ImageSizePreset::Small);
  std::filesystem::path cached_path;
  {
    auto opened =
        ImageCacheStore::open(make_config(root, 0), root.parent_path());
    CHECK(opened.has_value());
    auto store = std::move(opened.value());
    CHECK(write_payload(store, key, "0123456789", nullptr));
    auto found = store.find(key);
    CHECK(found.has_value());
    CHECK(found->has_value());
    cached_path = (*found)->file_path();
  }

  CHECK(std::filesystem::exists(cached_path));
  std::filesystem::remove(cached_path);
  const auto orphan_dir = root / "zz";
  std::filesystem::create_directories(orphan_dir);
  const auto orphan = orphan_dir / "orphan.bin";
  {
    std::ofstream out(orphan, std::ios::binary);
    out << "orphan";
  }

  {
    auto opened =
        ImageCacheStore::open(make_config(root, 0), root.parent_path());
    CHECK(opened.has_value());
    auto store = std::move(opened.value());
    CHECK(!exists(store, key));
    CHECK(!std::filesystem::exists(orphan));
    CHECK(write_payload(store, key, "rebuilt", nullptr));
    CHECK(exists(store, key));
  }
  std::filesystem::remove_all(root);
  return true;
}

bool test_concurrent_same_key() {
  const auto root = make_root("concurrent");
  {
    auto opened =
        ImageCacheStore::open(make_config(root, 0), root.parent_path());
    CHECK(opened.has_value());
    auto store = std::move(opened.value());
    const auto key = make_key("img-concurrent", ImageSizePreset::Regular);

    constexpr int kThreads = 8;
    constexpr int kIterations = 5;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int thread = 0; thread < kThreads; ++thread) {
      threads.emplace_back([&store, key, thread]() {
        const std::string payload =
            "thread-" + std::to_string(thread) + std::string(256, 'x');
        for (int i = 0; i < kIterations; ++i) {
          auto found = store.find(key);
          if (found.has_value() && found->has_value()) {
            std::string content;
            read_all(**found, content);
            continue;
          }
          write_payload(store, key, payload, nullptr);
        }
      });
    }
    for (auto &thread : threads) {
      thread.join();
    }

    auto found = store.find(key);
    CHECK(found.has_value());
    CHECK(found->has_value());
    std::string content;
    CHECK(read_all(**found, content));
    CHECK(content.rfind("thread-", 0) == 0);
    CHECK(content.size() >= 7 + 1 + 256);
  }
  std::filesystem::remove_all(root);
  return true;
}

} // namespace

int main() {
  const bool ok = test_miss() && test_write_commit_read() &&
                  test_abort_cleanup() && test_eviction_priority() &&
                  test_last_hit_lru() && test_skip_in_use() &&
                  test_oversize_skip() && test_self_heal() &&
                  test_reconcile() && test_concurrent_same_key();
  if (!ok || g_failures != 0) {
    std::cerr << "cache smoke failed: " << g_failures << " failure(s)\n";
    return 1;
  }
  std::cout << "cache smoke passed\n";
  return 0;
}
