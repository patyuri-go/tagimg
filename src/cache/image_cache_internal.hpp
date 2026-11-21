#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include <soci/soci.h>

#include "cache/image_cache_object.hpp"
#include "common/result.hpp"

namespace tagimg::cache_detail {

struct EntryRow {
  std::string cache_key;
  std::string file_path;
  std::uint64_t size_bytes = 0;
  std::string content_type;
};

// 缓存仓库的共享状态。读/写对象可能比 ImageCacheStore 活得更久，所以
// 对象里保存 shared_ptr。
struct SharedState {
  std::filesystem::path root;
  std::uint64_t max_bytes = 0;
  soci::session sql;
  std::mutex mutex;
  std::unordered_map<std::string, int> readers;
  std::unordered_map<std::string, int> writers;
  std::uint64_t total_bytes = 0;
};

// 一个读或写句柄的内部状态。
struct ObjectImpl {
  std::shared_ptr<SharedState> state;
  ImageCacheKey key;
  std::string canonical_key;
  std::string hash;
  std::string extension;
  std::string content_type;
  std::uint64_t size_bytes = 0;
  std::filesystem::path path;
  std::filesystem::path temporary_path;
  bool reader = false;
  bool writer = false;
  bool released = false;
  bool committed = false;
  std::unique_ptr<std::ifstream> input;
  std::unique_ptr<std::ofstream> output;

  ~ObjectImpl();
};

[[nodiscard]] std::int64_t now_millis() noexcept;

// 以下两个 acquire 要求调用方已经持有 state.mutex。
void acquire_reader_locked(SharedState &state, const std::string &key);
void acquire_writer_locked(SharedState &state, const std::string &key);

// 自带加锁，可在任意位置调用。
void release_use(SharedState &state, const std::string &key,
                 bool reader) noexcept;

// 要求调用方已经持有 state.mutex。
void release_use_locked(SharedState &state, const std::string &key,
                        bool reader) noexcept;

[[nodiscard]] bool is_in_use_locked(const SharedState &state,
                                    const std::string &key) noexcept;

// 在缓存根目录下生成正式文件相对路径：<hash前2位>/<hash>.<ext>。
[[nodiscard]] std::string relative_cache_path(std::string_view hash,
                                              std::string_view extension);

// 把已写完的临时文件搬入正式位置并更新索引，必要时淘汰。
[[nodiscard]] Result<ImageCacheCommit, ImageCacheError>
commit_entry(const std::shared_ptr<SharedState> &state,
             const std::string &canonical_key, const std::string &hash,
             const std::string &extension, std::string content_type,
             int priority, const std::filesystem::path &temporary_path,
             std::uint64_t size_bytes);

// 读路径发现文件缺失 / 大小不符时删除索引行。要求调用方持有 mutex。
void self_heal_locked(SharedState &state, const std::string &canonical_key,
                      const std::string &relative_path,
                      std::uint64_t size_bytes) noexcept;

} // namespace tagimg::cache_detail
