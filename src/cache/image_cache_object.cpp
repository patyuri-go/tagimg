#include "cache/image_cache_internal.hpp"

#include <fstream>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace tagimg {
namespace {

bool fsync_file(const std::filesystem::path &path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  const int result = ::fsync(fd);
  ::close(fd);
  return result == 0;
}

} // namespace

std::string_view to_string(ImageCacheError error) noexcept {
  switch (error) {
  case ImageCacheError::NotEnabled:
    return "NotEnabled";
  case ImageCacheError::OpenFailed:
    return "OpenFailed";
  case ImageCacheError::SchemaFailed:
    return "SchemaFailed";
  case ImageCacheError::QueryFailed:
    return "QueryFailed";
  case ImageCacheError::IoFailed:
    return "IoFailed";
  case ImageCacheError::InvalidKey:
    return "InvalidKey";
  }
  return "Unknown";
}

namespace cache_detail {

ObjectImpl::~ObjectImpl() {
  if (output) {
    output->close();
    output.reset();
  }
  if (writer && !committed && !temporary_path.empty()) {
    std::error_code error;
    std::filesystem::remove(temporary_path, error);
  }
  if (!released && state) {
    if (reader) {
      release_use(*state, canonical_key, true);
    } else if (writer) {
      release_use(*state, canonical_key, false);
    }
    released = true;
  }
}

} // namespace cache_detail

StoredImageCacheObject::StoredImageCacheObject(
    std::unique_ptr<cache_detail::ObjectImpl> impl)
    : impl_(std::move(impl)) {
}

StoredImageCacheObject::~StoredImageCacheObject() = default;

StoredImageCacheObject::StoredImageCacheObject(
    StoredImageCacheObject &&other) noexcept = default;

StoredImageCacheObject &StoredImageCacheObject::operator=(
    StoredImageCacheObject &&other) noexcept = default;

bool StoredImageCacheObject::readable() const noexcept {
  return impl_ && impl_->input != nullptr;
}

bool StoredImageCacheObject::writable() const noexcept {
  return impl_ && impl_->writer && !impl_->committed && impl_->output;
}

std::istream &StoredImageCacheObject::read() {
  if (!readable()) {
    throw std::logic_error(
        "StoredImageCacheObject::read on non-readable object");
  }
  return *impl_->input;
}

std::ostream &StoredImageCacheObject::write() {
  if (!writable()) {
    throw std::logic_error(
        "StoredImageCacheObject::write on non-writable object");
  }
  return *impl_->output;
}

Result<void, ImageCacheError> StoredImageCacheObject::flush() {
  if (!impl_ || !impl_->writer || impl_->committed || !impl_->output) {
    return ImageCacheError::IoFailed;
  }
  impl_->output->flush();
  if (!impl_->output->good()) {
    return ImageCacheError::IoFailed;
  }
  return {};
}

Result<ImageCacheCommit, ImageCacheError> StoredImageCacheObject::commit() {
  if (!impl_) {
    return ImageCacheError::IoFailed;
  }
  auto &impl = *impl_;
  if (!impl.writer) {
    throw std::logic_error("StoredImageCacheObject::commit on read object");
  }
  if (impl.committed) {
    return ImageCacheCommit::Committed;
  }
  if (!impl.output) {
    return ImageCacheError::IoFailed;
  }

  // 禁用状态仍允许 create()/write()，但提交时只清理临时文件。
  if (!impl.state->enabled) {
    impl.output->flush();
    impl.output->close();
    impl.output.reset();
    std::error_code remove_error;
    std::filesystem::remove(impl.temporary_path, remove_error);
    impl.released = true;
    cache_detail::release_use(*impl.state, impl.canonical_key, false);
    return ImageCacheError::NotEnabled;
  }

  impl.output->flush();
  if (!impl.output->good()) {
    return ImageCacheError::IoFailed;
  }
  impl.output->close();
  if (impl.output->fail()) {
    return ImageCacheError::IoFailed;
  }
  impl.output.reset();

  std::error_code error;
  const auto size = std::filesystem::file_size(impl.temporary_path, error);
  if (error) {
    return ImageCacheError::IoFailed;
  }
  if (!fsync_file(impl.temporary_path)) {
    return ImageCacheError::IoFailed;
  }

  if (impl.content_type.empty()) {
    impl.content_type = std::string(infer_content_type(impl.extension));
  }

  const int priority = preset_priority(impl.key.params.preset);
  auto result = cache_detail::commit_entry(
      impl.state, impl.canonical_key, impl.hash, impl.extension,
      impl.content_type, priority, impl.temporary_path,
      static_cast<std::uint64_t>(size));
  if (result.has_error()) {
    return result.error();
  }

  impl.size_bytes = static_cast<std::uint64_t>(size);
  impl.committed = true;
  impl.released = true;
  cache_detail::release_use(*impl.state, impl.canonical_key, false);
  return result.value();
}

Result<void, ImageCacheError> StoredImageCacheObject::pin() {
  if (!impl_) {
    return ImageCacheError::IoFailed;
  }
  auto &impl = *impl_;
  if (impl.reader) {
    return {};
  }
  if (!impl.writer || !impl.committed || !impl.state) {
    return ImageCacheError::IoFailed;
  }

  const auto state = impl.state;
  std::unique_lock<std::mutex> lock(state->mutex);

  std::string relative;
  std::string content_type;
  std::uint64_t stored_size = 0;
  try {
    soci::rowset<soci::row> rows =
        (state->sql.prepare
             << "SELECT file_path, size_bytes, content_type FROM image_cache "
                "WHERE cache_key = :key",
         soci::use(impl.canonical_key));
    auto it = rows.begin();
    if (it == rows.end()) {
      return ImageCacheError::IoFailed;
    }
    relative = (*it).get<std::string>(0);
    stored_size = static_cast<std::uint64_t>((*it).get<long long>(1));
    content_type = (*it).get<std::string>(2);
  } catch (const std::exception &) {
    return ImageCacheError::QueryFailed;
  }

  cache_detail::acquire_reader_locked(*state, impl.canonical_key);

  const auto absolute = state->root / relative;
  std::error_code error;
  const auto actual = std::filesystem::file_size(absolute, error);
  if (error || actual != stored_size) {
    cache_detail::self_heal_locked(*state, impl.canonical_key, relative,
                                   stored_size);
    cache_detail::release_use_locked(*state, impl.canonical_key, true);
    return ImageCacheError::IoFailed;
  }

  auto input = std::make_unique<std::ifstream>(absolute, std::ios::binary);
  if (!*input) {
    cache_detail::self_heal_locked(*state, impl.canonical_key, relative,
                                   stored_size);
    cache_detail::release_use_locked(*state, impl.canonical_key, true);
    return ImageCacheError::IoFailed;
  }

  impl.reader = true;
  impl.released = false;
  impl.path = absolute;
  impl.size_bytes = stored_size;
  impl.content_type = std::move(content_type);
  impl.input = std::move(input);
  return {};
}

bool StoredImageCacheObject::remove() {
  if (!impl_ || !impl_->state || !impl_->state->enabled) {
    return false;
  }
  auto &impl = *impl_;
  if (impl.writer && !impl.committed) {
    return false;
  }

  const auto state = impl.state;
  std::lock_guard<std::mutex> lock(state->mutex);

  std::string relative;
  std::uint64_t stored_size = 0;
  try {
    soci::rowset<soci::row> rows =
        (state->sql.prepare
             << "SELECT file_path, size_bytes FROM image_cache WHERE "
                "cache_key = :key",
         soci::use(impl.canonical_key));
    auto it = rows.begin();
    if (it == rows.end()) {
      return false;
    }
    relative = (*it).get<std::string>(0);
    stored_size = static_cast<std::uint64_t>((*it).get<long long>(1));
  } catch (const std::exception &) {
    return false;
  }

  const auto reader_it = state->readers.find(impl.canonical_key);
  const int reader_count =
      reader_it == state->readers.end() ? 0 : reader_it->second;
  const int own_reader = impl.reader ? 1 : 0;
  if (reader_count > own_reader ||
      state->writers.find(impl.canonical_key) != state->writers.end()) {
    return false;
  }

  try {
    state->sql << "DELETE FROM image_cache WHERE cache_key = :key",
        soci::use(impl.canonical_key);
  } catch (const std::exception &) {
    return false;
  }
  if (state->total_bytes >= stored_size) {
    state->total_bytes -= stored_size;
  } else {
    state->total_bytes = 0;
  }

  std::error_code remove_error;
  std::filesystem::remove(state->root / relative, remove_error);

  if (impl.reader) {
    cache_detail::release_use_locked(*state, impl.canonical_key, true);
    impl.reader = false;
    impl.released = true;
    impl.input.reset();
  }
  return true;
}

const ImageCacheKey &StoredImageCacheObject::key() const noexcept {
  static const ImageCacheKey kEmpty;
  return impl_ ? impl_->key : kEmpty;
}

const std::string &StoredImageCacheObject::content_type() const noexcept {
  static const std::string kEmpty;
  return impl_ ? impl_->content_type : kEmpty;
}

std::uint64_t StoredImageCacheObject::size() const noexcept {
  return impl_ ? impl_->size_bytes : 0;
}

const std::filesystem::path &
StoredImageCacheObject::file_path() const noexcept {
  static const std::filesystem::path kEmpty;
  return impl_ ? impl_->path : kEmpty;
}

const std::filesystem::path &
StoredImageCacheObject::temporary_path() const noexcept {
  static const std::filesystem::path kEmpty;
  return impl_ ? impl_->temporary_path : kEmpty;
}

void StoredImageCacheObject::set_content_type(
    std::string_view content_type) noexcept {
  if (impl_) {
    impl_->content_type = std::string(content_type);
  }
}

} // namespace tagimg
