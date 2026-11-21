#include "cache/image_cache_store.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <soci/sqlite3/soci-sqlite3.h>
#include <unistd.h>

#include "cache/image_cache_internal.hpp"

namespace tagimg {
namespace {

constexpr const char *kCreateTableSql = R"sql(
CREATE TABLE IF NOT EXISTS image_cache (
  cache_key    TEXT PRIMARY KEY,
  file_path    TEXT NOT NULL,
  size_bytes   INTEGER NOT NULL,
  last_hit_at  INTEGER NOT NULL,
  priority     INTEGER NOT NULL,
  cache_type   TEXT NOT NULL,
  content_type TEXT NOT NULL DEFAULT ''
) WITHOUT ROWID
)sql";

constexpr const char *kCreateIndexSql = R"sql(
CREATE INDEX IF NOT EXISTS idx_image_cache_evict
  ON image_cache(priority ASC, last_hit_at ASC)
)sql";

constexpr const char *kFindSql = R"sql(
UPDATE image_cache
   SET last_hit_at = :now
 WHERE cache_key = :key
RETURNING file_path, size_bytes, content_type
)sql";

constexpr const char *kUpsertSql = R"sql(
INSERT INTO image_cache
  (cache_key, file_path, size_bytes, last_hit_at, priority, cache_type,
   content_type)
VALUES
  (:key, :path, :size, :now, :priority, :type, :content_type)
ON CONFLICT(cache_key) DO UPDATE SET
  file_path    = excluded.file_path,
  size_bytes   = excluded.size_bytes,
  last_hit_at  = excluded.last_hit_at,
  priority     = excluded.priority,
  cache_type   = excluded.cache_type,
  content_type = excluded.content_type
)sql";

constexpr const char *kSumSql =
    "SELECT COALESCE(SUM(size_bytes), 0) FROM image_cache";

std::string unique_suffix() {
  static std::atomic<std::uint64_t> counter{0};
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::to_string(::getpid()) + "." + std::to_string(now) + "." +
         std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

bool is_database_file(std::string_view relative_path) {
  return relative_path == "index.sqlite3" ||
         relative_path == "index.sqlite3-wal" ||
         relative_path == "index.sqlite3-shm" ||
         relative_path == "index.sqlite3-journal";
}

void clear_directory(const std::filesystem::path &directory) {
  std::error_code error;
  if (!std::filesystem::exists(directory, error)) {
    return;
  }
  for (auto it = std::filesystem::directory_iterator(directory, error);
       !error && it != std::filesystem::directory_iterator();
       it.increment(error)) {
    std::error_code remove_error;
    std::filesystem::remove_all(it->path(), remove_error);
  }
}

Result<std::uint64_t, ImageCacheError>
query_total_bytes(cache_detail::SharedState &state) {
  try {
    long long total = 0;
    state.sql << kSumSql, soci::into(total);
    return total > 0 ? static_cast<std::uint64_t>(total) : 0;
  } catch (const std::exception &) {
    return ImageCacheError::QueryFailed;
  }
}

Result<void, ImageCacheError> setup_schema(cache_detail::SharedState &state,
                                           bool &table_existed) {
  try {
    std::string journal_mode;
    state.sql << "PRAGMA journal_mode=WAL", soci::into(journal_mode);
    int busy_timeout = 0;
    state.sql << "PRAGMA busy_timeout=5000", soci::into(busy_timeout);
    state.sql << "PRAGMA synchronous=NORMAL";
  } catch (const std::exception &) {
    return ImageCacheError::OpenFailed;
  }

  try {
    int count = 0;
    state.sql << "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' "
                 "AND name = 'image_cache'",
        soci::into(count);
    table_existed = count > 0;
  } catch (const std::exception &) {
    return ImageCacheError::OpenFailed;
  }

  try {
    state.sql << kCreateTableSql;
    state.sql << kCreateIndexSql;
  } catch (const std::exception &) {
    return ImageCacheError::SchemaFailed;
  }
  return {};
}

Result<void, ImageCacheError> reconcile(cache_detail::SharedState &state) {
  std::unordered_set<std::string> indexed;
  std::vector<std::string> dead_keys;

  try {
    soci::rowset<soci::row> rows =
        (state.sql.prepare
         << "SELECT cache_key, file_path, size_bytes FROM image_cache");
    for (auto it = rows.begin(); it != rows.end(); ++it) {
      const std::string cache_key = (*it).get<std::string>(0);
      const std::string file_path = (*it).get<std::string>(1);
      const auto size = static_cast<std::uint64_t>((*it).get<long long>(2));

      std::error_code error;
      const auto actual =
          std::filesystem::file_size(state.root / file_path, error);
      if (error || actual != size) {
        dead_keys.push_back(cache_key);
      } else {
        indexed.insert(file_path);
      }
    }
  } catch (const std::exception &) {
    return ImageCacheError::QueryFailed;
  }

  if (!dead_keys.empty()) {
    try {
      soci::transaction transaction(state.sql);
      for (const auto &key : dead_keys) {
        state.sql << "DELETE FROM image_cache WHERE cache_key = :key",
            soci::use(key);
      }
      transaction.commit();
    } catch (const std::exception &) {
      return ImageCacheError::QueryFailed;
    }
  }

  std::vector<std::filesystem::path> orphans;
  std::error_code error;
  for (auto it =
           std::filesystem::recursive_directory_iterator(state.root, error);
       !error && it != std::filesystem::recursive_directory_iterator();
       it.increment(error)) {
    if (it->is_symlink(error) || !it->is_regular_file(error)) {
      continue;
    }
    const auto relative =
        std::filesystem::relative(it->path(), state.root, error)
            .generic_string();
    if (error) {
      error.clear();
      continue;
    }
    if (is_database_file(relative) || relative.rfind("tmp/", 0) == 0) {
      continue;
    }
    if (indexed.find(relative) == indexed.end()) {
      orphans.push_back(it->path());
    }
  }
  for (const auto &orphan : orphans) {
    std::error_code remove_error;
    std::filesystem::remove(orphan, remove_error);
  }

  clear_directory(state.root / "tmp");

  auto total = query_total_bytes(state);
  if (total.has_error()) {
    return total.error();
  }
  state.total_bytes = total.value();
  return {};
}

} // namespace

namespace cache_detail {

std::int64_t now_millis() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void acquire_reader_locked(SharedState &state, const std::string &key) {
  ++state.readers[key];
}

void acquire_writer_locked(SharedState &state, const std::string &key) {
  ++state.writers[key];
}

void release_use(SharedState &state, const std::string &key,
                 bool reader) noexcept {
  std::lock_guard<std::mutex> lock(state.mutex);
  release_use_locked(state, key, reader);
}

void release_use_locked(SharedState &state, const std::string &key,
                        bool reader) noexcept {
  auto &map = reader ? state.readers : state.writers;
  const auto it = map.find(key);
  if (it == map.end()) {
    return;
  }
  if (--it->second <= 0) {
    map.erase(it);
  }
}

bool is_in_use_locked(const SharedState &state,
                      const std::string &key) noexcept {
  return state.readers.find(key) != state.readers.end() ||
         state.writers.find(key) != state.writers.end();
}

std::string relative_cache_path(std::string_view hash,
                                std::string_view extension) {
  if (hash.size() < 2) {
    return {};
  }
  std::string path;
  path.reserve(hash.size() + extension.size() + 4);
  path.append(hash.substr(0, 2));
  path.push_back('/');
  path.append(hash);
  path.push_back('.');
  path.append(extension);
  return path;
}

Result<ImageCacheCommit, ImageCacheError>
commit_entry(const std::shared_ptr<SharedState> &state,
             const std::string &canonical_key, const std::string &hash,
             const std::string &extension, std::string content_type,
             int priority, const std::filesystem::path &temporary_path,
             std::uint64_t size_bytes) {
  std::unique_lock<std::mutex> lock(state->mutex);

  const bool limited = state->max_bytes != 0;
  if (limited && size_bytes > state->max_bytes) {
    std::error_code remove_error;
    std::filesystem::remove(temporary_path, remove_error);
    return ImageCacheCommit::SkippedOverLimit;
  }

  std::uint64_t old_size = 0;
  try {
    soci::rowset<soci::row> rows =
        (state->sql.prepare
             << "SELECT file_path, size_bytes FROM image_cache WHERE "
                "cache_key = :key",
         soci::use(canonical_key));
    auto it = rows.begin();
    if (it != rows.end()) {
      old_size = static_cast<std::uint64_t>((*it).get<long long>(1));
    }
  } catch (const std::exception &) {
    return ImageCacheError::QueryFailed;
  }

  struct Victim {
    std::string cache_key;
    std::string file_path;
    std::uint64_t size_bytes = 0;
  };
  std::vector<Victim> victims;
  std::uint64_t projected =
      state->total_bytes >= old_size ? state->total_bytes - old_size : 0;
  projected += size_bytes;

  if (limited && projected > state->max_bytes) {
    try {
      soci::rowset<soci::row> rows =
          (state->sql.prepare
           << "SELECT cache_key, file_path, size_bytes FROM image_cache "
              "ORDER BY priority ASC, last_hit_at ASC");
      for (auto it = rows.begin(); it != rows.end(); ++it) {
        const std::string cache_key = (*it).get<std::string>(0);
        if (cache_key == canonical_key) {
          continue;
        }
        if (is_in_use_locked(*state, cache_key)) {
          continue;
        }
        Victim victim;
        victim.cache_key = cache_key;
        victim.file_path = (*it).get<std::string>(1);
        victim.size_bytes = static_cast<std::uint64_t>((*it).get<long long>(2));
        const auto victim_size = victim.size_bytes;
        victims.push_back(std::move(victim));
        projected = projected >= victim_size ? projected - victim_size : 0;
        if (projected <= state->max_bytes) {
          break;
        }
      }
    } catch (const std::exception &) {
      return ImageCacheError::QueryFailed;
    }
    if (projected > state->max_bytes) {
      std::error_code remove_error;
      std::filesystem::remove(temporary_path, remove_error);
      return ImageCacheCommit::SkippedOverLimit;
    }
  }

  const std::string relative = relative_cache_path(hash, extension);
  if (relative.empty()) {
    return ImageCacheError::IoFailed;
  }
  const auto final_path = state->root / relative;
  std::error_code error;
  std::filesystem::create_directories(final_path.parent_path(), error);
  if (error) {
    return ImageCacheError::IoFailed;
  }
  std::filesystem::rename(temporary_path, final_path, error);
  if (error) {
    return ImageCacheError::IoFailed;
  }

  if (content_type.empty()) {
    content_type = std::string(infer_content_type(extension));
  }

  const std::string cache_type(kImageCacheType);
  const long long size_bind = static_cast<long long>(size_bytes);
  const long long now_bind = now_millis();

  try {
    soci::transaction transaction(state->sql);
    for (const auto &victim : victims) {
      state->sql << "DELETE FROM image_cache WHERE cache_key = :key",
          soci::use(victim.cache_key);
    }
    state->sql << kUpsertSql, soci::use(canonical_key, "key"),
        soci::use(relative, "path"), soci::use(size_bind, "size"),
        soci::use(now_bind, "now"), soci::use(priority, "priority"),
        soci::use(cache_type, "type"), soci::use(content_type, "content_type");
    transaction.commit();
  } catch (const std::exception &) {
    return ImageCacheError::QueryFailed;
  }

  std::uint64_t removed = old_size;
  for (const auto &victim : victims) {
    removed += victim.size_bytes;
  }
  state->total_bytes =
      state->total_bytes >= removed ? state->total_bytes - removed : 0;
  state->total_bytes += size_bytes;

  // 仍持锁时删除被淘汰的文件，避免同一 key 的新写入在删除前重新落盘。
  for (const auto &victim : victims) {
    std::error_code remove_error;
    std::filesystem::remove(state->root / victim.file_path, remove_error);
  }

  return ImageCacheCommit::Committed;
}

void self_heal_locked(SharedState &state, const std::string &canonical_key,
                      const std::string &relative_path,
                      std::uint64_t size_bytes) noexcept {
  try {
    std::string file_path;
    std::uint64_t stored_size = 0;
    soci::rowset<soci::row> rows =
        (state.sql.prepare
             << "SELECT file_path, size_bytes FROM image_cache WHERE "
                "cache_key = :key",
         soci::use(canonical_key));
    auto it = rows.begin();
    if (it == rows.end()) {
      return;
    }
    file_path = (*it).get<std::string>(0);
    stored_size = static_cast<std::uint64_t>((*it).get<long long>(1));
    if (file_path != relative_path) {
      return;
    }

    state.sql << "DELETE FROM image_cache WHERE cache_key = :key AND "
                 "file_path = :path",
        soci::use(canonical_key), soci::use(file_path);
    if (state.total_bytes >= stored_size) {
      state.total_bytes -= stored_size;
    } else {
      state.total_bytes = 0;
    }
  } catch (const std::exception &) {
  }
  (void)size_bytes;
}

} // namespace cache_detail

struct ImageCacheStore::Impl {
  std::shared_ptr<cache_detail::SharedState> state;
};

ImageCacheStore::ImageCacheStore(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {
}

ImageCacheStore::~ImageCacheStore() = default;

ImageCacheStore::ImageCacheStore(ImageCacheStore &&other) noexcept = default;

ImageCacheStore &
ImageCacheStore::operator=(ImageCacheStore &&other) noexcept = default;

Result<ImageCacheStore, ImageCacheError>
ImageCacheStore::open(const ImageCacheConfig &config,
                      const std::filesystem::path &base_dir) {
  if (!config.enabled) {
    return ImageCacheError::NotEnabled;
  }

  std::filesystem::path root = config.directory;
  if (root.is_relative()) {
    root = base_dir / root;
  }

  std::error_code error;
  std::filesystem::create_directories(root, error);
  if (error) {
    return ImageCacheError::OpenFailed;
  }
  std::filesystem::create_directories(root / "tmp", error);
  if (error) {
    return ImageCacheError::OpenFailed;
  }

  auto state = std::make_shared<cache_detail::SharedState>();
  state->root = root;
  state->max_bytes = config.max_bytes;

  try {
    state->sql.open(soci::sqlite3, (root / "index.sqlite3").string());
  } catch (const std::exception &) {
    return ImageCacheError::OpenFailed;
  }

  bool table_existed = false;
  auto schema = setup_schema(*state, table_existed);
  if (schema.has_error()) {
    return schema.error();
  }

  if (!table_existed) {
    clear_directory(root / "tmp");
    state->total_bytes = 0;
  } else if (config.reconcile_on_open) {
    auto result = reconcile(*state);
    if (result.has_error()) {
      return result.error();
    }
  } else {
    auto total = query_total_bytes(*state);
    if (total.has_error()) {
      return total.error();
    }
    state->total_bytes = total.value();
  }

  auto impl = std::make_unique<Impl>();
  impl->state = std::move(state);
  return ImageCacheStore(std::move(impl));
}

Result<std::optional<StoredImageCacheObject>, ImageCacheError>
ImageCacheStore::find(const ImageCacheKey &key) {
  if (!impl_ || !impl_->state) {
    return ImageCacheError::OpenFailed;
  }
  if (!is_valid_cache_key(key)) {
    return ImageCacheError::InvalidKey;
  }

  const auto state = impl_->state;
  const std::string canonical = canonical_cache_key(key);
  const std::string hash = cache_key_hash(canonical);
  if (hash.empty()) {
    return ImageCacheError::IoFailed;
  }
  const std::string extension = normalize_extension(key.params.extension);

  std::unique_lock<std::mutex> lock(state->mutex);

  std::string relative;
  std::string content_type;
  std::uint64_t stored_size = 0;
  const long long now = cache_detail::now_millis();
  try {
    soci::rowset<soci::row> rows =
        (state->sql.prepare << kFindSql, soci::use(now), soci::use(canonical));
    auto it = rows.begin();
    if (it == rows.end()) {
      return std::optional<StoredImageCacheObject>{};
    }
    relative = (*it).get<std::string>(0);
    stored_size = static_cast<std::uint64_t>((*it).get<long long>(1));
    content_type = (*it).get<std::string>(2);
  } catch (const std::exception &) {
    return ImageCacheError::QueryFailed;
  }

  cache_detail::acquire_reader_locked(*state, canonical);

  const auto absolute = state->root / relative;
  std::error_code error;
  const auto actual = std::filesystem::file_size(absolute, error);
  if (error || actual != stored_size) {
    cache_detail::self_heal_locked(*state, canonical, relative, stored_size);
    cache_detail::release_use_locked(*state, canonical, true);
    return std::optional<StoredImageCacheObject>{};
  }

  auto input = std::make_unique<std::ifstream>(absolute, std::ios::binary);
  if (!*input) {
    cache_detail::self_heal_locked(*state, canonical, relative, stored_size);
    cache_detail::release_use_locked(*state, canonical, true);
    return std::optional<StoredImageCacheObject>{};
  }

  auto object = std::make_unique<cache_detail::ObjectImpl>();
  object->state = state;
  object->key = key;
  object->canonical_key = canonical;
  object->hash = hash;
  object->extension = extension;
  object->content_type = std::move(content_type);
  object->size_bytes = stored_size;
  object->path = absolute;
  object->reader = true;
  object->input = std::move(input);

  return std::optional<StoredImageCacheObject>(
      StoredImageCacheObject(std::move(object)));
}

Result<StoredImageCacheObject, ImageCacheError>
ImageCacheStore::create(const ImageCacheKey &key) {
  if (!impl_ || !impl_->state) {
    return ImageCacheError::OpenFailed;
  }
  if (!is_valid_cache_key(key)) {
    return ImageCacheError::InvalidKey;
  }

  const auto state = impl_->state;
  const std::string canonical = canonical_cache_key(key);
  const std::string hash = cache_key_hash(canonical);
  if (hash.empty()) {
    return ImageCacheError::IoFailed;
  }
  const std::string extension = normalize_extension(key.params.extension);

  std::lock_guard<std::mutex> lock(state->mutex);

  std::error_code error;
  std::filesystem::create_directories(state->root / "tmp", error);
  if (error) {
    return ImageCacheError::IoFailed;
  }

  const auto temporary =
      state->root / "tmp" /
      (hash + "." + unique_suffix() + "." + extension + ".part");
  auto output = std::make_unique<std::ofstream>(temporary, std::ios::binary |
                                                               std::ios::trunc);
  if (!*output) {
    return ImageCacheError::IoFailed;
  }

  cache_detail::acquire_writer_locked(*state, canonical);

  auto object = std::make_unique<cache_detail::ObjectImpl>();
  object->state = state;
  object->key = key;
  object->canonical_key = canonical;
  object->hash = hash;
  object->extension = extension;
  object->content_type = key.params.content_type;
  object->path =
      state->root / cache_detail::relative_cache_path(hash, extension);
  object->temporary_path = temporary;
  object->writer = true;
  object->output = std::move(output);

  return StoredImageCacheObject(std::move(object));
}

} // namespace tagimg
