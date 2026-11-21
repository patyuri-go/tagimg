#include "job/scan_job.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <span>
#include <utility>
#include <vector>

#include "cache/image_cache_keys.hpp"
#include "cache/image_cache_object.hpp"
#include "cache/image_cache_store.hpp"
#include "common/uuid.hpp"
#include "image/image_metadata.hpp"
#include "io/byte_stream.hpp"
#include "io/temp_file.hpp"
#include "job/ai_preflight.hpp"

namespace tagimg {
namespace {

constexpr std::size_t kReadBufferSize = 64 * 1024;

JobError map_stream_error(ByteStreamError error) {
  switch (error) {
  case ByteStreamError::TooLarge:
    return JobError::ImageTooLarge;
  case ByteStreamError::InvalidUrl:
  case ByteStreamError::ConnectFailed:
  case ByteStreamError::Timeout:
  case ByteStreamError::HttpError:
  case ByteStreamError::TransferFailed:
  case ByteStreamError::ReadFailed:
    return JobError::DownloadFailed;
  }
  return JobError::DownloadFailed;
}

JobError map_tagger_error(TaggerError error) {
  switch (error) {
  case TaggerError::SidecarUnavailable:
  case TaggerError::Timeout:
    return JobError::AiUnavailable;
  case TaggerError::ImageTooLarge:
    return JobError::ImageTooLarge;
  case TaggerError::FileReadError:
    return JobError::FileError;
  case TaggerError::InvalidImage:
  case TaggerError::HttpError:
  case TaggerError::InvalidResponse:
    return JobError::TaggingFailed;
  }
  return JobError::TaggingFailed;
}

JobError map_embedder_error(EmbedderError error) {
  switch (error) {
  case EmbedderError::SidecarUnavailable:
  case EmbedderError::Timeout:
    return JobError::AiUnavailable;
  case EmbedderError::ImageTooLarge:
    return JobError::ImageTooLarge;
  case EmbedderError::FileReadError:
    return JobError::FileError;
  case EmbedderError::InvalidImage:
  case EmbedderError::InvalidText:
  case EmbedderError::HttpError:
  case EmbedderError::InvalidResponse:
  case EmbedderError::DimensionMismatch:
    return JobError::EmbeddingFailed;
  }
  return JobError::EmbeddingFailed;
}

JobError map_tagimg_lookup_error(TagimgRepositoryError error) {
  switch (error) {
  case TagimgRepositoryError::InvalidArgument:
    return JobError::InvalidEntityId;
  case TagimgRepositoryError::ConnectionFailed:
  case TagimgRepositoryError::QueryFailed:
    return JobError::RepositoryFailed;
  default:
    return JobError::RepositoryFailed;
  }
}

JobError map_metadata_error(ImageMetadataError error) {
  switch (error) {
  case ImageMetadataError::InitializationFailed:
    return JobError::InternalError;
  case ImageMetadataError::OpenFailed:
    return JobError::NotImage;
  }
  return JobError::InternalError;
}

} // namespace

ScanJob::ScanJob(std::string entity_id) : entity_id_(std::move(entity_id)) {
}

ScanJob::ScanJob(std::string entity_id, std::string image_id)
    : entity_id_(std::move(entity_id)), image_id_(std::move(image_id)) {
}

Result<ScanResult, JobError>
ScanJob::scan_entity(JobContext &context, const std::string &entity_id) {
  AiPreflight ai;
  return scan_entity(context, entity_id, ai);
}

Result<ScanResult, JobError>
ScanJob::scan_entity(JobContext &context, const std::string &entity_id,
                     AiPreflight &ai) {
  ScanJob job(entity_id);
  return job.scan(context, true, ai);
}

Result<ScanResult, JobError>
ScanJob::rescan_entity(JobContext &context, const std::string &entity_id,
                       const std::string &image_id) {
  AiPreflight ai;
  return rescan_entity(context, entity_id, image_id, ai);
}

Result<ScanResult, JobError>
ScanJob::rescan_entity(JobContext &context, const std::string &entity_id,
                       const std::string &image_id, AiPreflight &ai) {
  ScanJob job(entity_id, image_id);
  return job.scan(context, false, ai);
}

StepResult ScanJob::step(JobContext &context, const JobRun &run) {
  (void)run;
  AiPreflight ai;
  result_ = scan(context, true, ai);
  return StepResult::Done;
}

Result<JobOutcome, JobError> ScanJob::take_outcome() {
  if (!result_) {
    return JobError::InternalError;
  }
  if (!*result_) {
    return result_->error();
  }
  return JobOutcome(std::move(**result_));
}

Result<ScanResult, JobError> ScanJob::scan(JobContext &context,
                                           bool check_duplicate,
                                           AiPreflight &ai) {
  if (!is_uuid(entity_id_)) {
    return JobError::InvalidEntityId;
  }

  // 在下载和推理之前判断实体是否已经入库，避免重复扫描。
  if (check_duplicate) {
    auto exists =
        context.tagimg_repository.get().image_exists_by_entity_id(entity_id_);
    if (!exists) {
      return map_tagimg_lookup_error(exists.error());
    }
    if (*exists) {
      return JobError::DuplicateImage;
    }
  }

  // 下载前确认 AI 可用。这里的预检按 job 缓存：批量 add 的多次调用
  // 只会真正请求一次 /health。
  if (!ai.ensure(context)) {
    return JobError::AiUnavailable;
  }

  return scan_content(context);
}

Result<ScanResult, JobError>
ScanJob::scan_content(JobContext &context) {
  const std::string url = entity_content_url(context.storage, entity_id_);

  ByteStreamOptions options;
  options.max_bytes = WdTagger::kMaxImageBytes;

  auto stream = HttpByteStream::open(url, options);
  if (!stream) {
    return map_stream_error(stream.error());
  }

  // 拿到响应头后才能判断长度。
  const auto started = (*stream)->wait_started();
  if (!started) {
    if (started.error() == ByteStreamError::HttpError &&
        (*stream)->http_status() == 404) {
      return JobError::EntityNotFound;
    }
    return map_stream_error(started.error());
  }

  const auto content_length = (*stream)->content_length();
  if (content_length && *content_length > WdTagger::kMaxImageBytes) {
    return JobError::ImageTooLarge;
  }

  // 重扫时优先把下载内容直接写入源缓存；初次扫描没有 image_id，仍用临时文件。
  std::shared_ptr<StoredImageCacheObject> source_cache;
  if (image_id_ && context.image_cache && context.image_cache->enabled()) {
    auto created =
        context.image_cache->create(make_source_cache_key(*image_id_));
    if (created) {
      source_cache = std::make_shared<StoredImageCacheObject>(std::move(*created));
    } else {
      std::cerr << "scan job: source cache create failed: "
                << to_string(created.error()) << '\n';
    }
  }

  std::optional<TempFile> file;
  if (!source_cache) {
    auto created = TempFile::create(context.work_dir, ".img");
    if (!created) {
      return JobError::FileError;
    }
    file.emplace(std::move(*created));
  }

  // 只缓存前 64KB 用于格式探测；超过后释放，避免整图驻留内存。
  std::vector<std::byte> prefix;
  prefix.reserve(kReadBufferSize);
  bool probed = false;

  std::array<std::byte, kReadBufferSize> buffer{};
  std::uint64_t bytes = 0;
  while (true) {
    auto chunk = (*stream)->read(buffer);
    if (!chunk) {
      return map_stream_error(chunk.error());
    }
    if (*chunk == 0) {
      break;
    }
    if (source_cache) {
      auto &out = source_cache->write();
      out.write(reinterpret_cast<const char *>(buffer.data()),
                static_cast<std::streamsize>(*chunk));
      if (!out.good()) {
        return JobError::FileError;
      }
    } else {
      auto written =
          file->write(std::span<const std::byte>(buffer.data(), *chunk));
      if (!written) {
        return JobError::FileError;
      }
    }
    bytes += *chunk;

    if (!probed) {
      const std::size_t remaining = kReadBufferSize - prefix.size();
      const std::size_t take = std::min(*chunk, remaining);
      prefix.insert(prefix.end(), buffer.begin(), buffer.begin() + take);
      if (prefix.size() >= kReadBufferSize) {
        auto supported = probe_supported_image(prefix);
        if (!supported) {
          return map_metadata_error(supported.error());
        }
        if (!*supported) {
          return JobError::NotImage;
        }
        probed = true;
        prefix.clear();
        prefix.shrink_to_fit();
      }
    }
  }

  if (!probed) {
    auto supported = probe_supported_image(prefix);
    if (!supported) {
      return map_metadata_error(supported.error());
    }
    if (!*supported) {
      return JobError::NotImage;
    }
  }

  std::filesystem::path read_path;
  if (source_cache) {
    const auto flushed = source_cache->flush();
    if (!flushed) {
      return JobError::FileError;
    }
    read_path = source_cache->temporary_path();
  } else {
    auto closed = file->close();
    if (!closed) {
      return JobError::FileError;
    }
    read_path = file->path();
  }

  auto metadata = read_image_metadata(read_path);
  if (!metadata) {
    return map_metadata_error(metadata.error());
  }
  if (source_cache && metadata->mime_type) {
    source_cache->set_content_type(*metadata->mime_type);
  }

  auto tags = context.tagger.get().tag_file(read_path);
  if (!tags) {
    return map_tagger_error(tags.error());
  }

  // 复用同一个文件生成向量，不重复下载。
  auto embedding = context.embedder.get().embed_file(read_path);
  if (!embedding) {
    // CLIP 向量是入库前置条件，缺失直接让整单失败。
    return map_embedder_error(embedding.error());
  }

  ScanResult result;
  result.entity_id = entity_id_;
  result.bytes = bytes;
  result.tags = std::move(*tags);
  result.metadata = std::move(*metadata);
  result.source_cache = std::move(source_cache);
  if (embedding) {
    result.embedding = std::move(embedding->values);
  }
  return result;
}

} // namespace tagimg
