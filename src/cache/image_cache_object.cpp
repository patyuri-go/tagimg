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

} // namespace tagimg
