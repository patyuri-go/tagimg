#include "job/scan_job.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include "common/uuid.hpp"
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

// 只按实体库记录的 mime_type 判断是否为图片。
bool is_image_mime(std::string_view mime_type) {
  constexpr std::string_view kPrefix = "image/";
  if (mime_type.size() < kPrefix.size()) {
    return false;
  }
  for (std::size_t index = 0; index < kPrefix.size(); ++index) {
    const auto actual = static_cast<unsigned char>(mime_type[index]);
    if (std::tolower(actual) != static_cast<unsigned char>(kPrefix[index])) {
      return false;
    }
  }
  return true;
}

// 去掉 Content-Type 的参数部分和首尾空白，得到纯媒体类型。
std::string media_type(std::string_view content_type) {
  const std::size_t semicolon = content_type.find(';');
  std::string_view value = content_type.substr(0, semicolon);
  while (!value.empty() &&
         (value.front() == ' ' || value.front() == '\t' ||
          value.front() == '\r' || value.front() == '\n')) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         (value.back() == ' ' || value.back() == '\t' ||
          value.back() == '\r' || value.back() == '\n')) {
    value.remove_suffix(1);
  }
  return std::string(value);
}

} // namespace

ScanJob::ScanJob(std::string entity_id) : entity_id_(std::move(entity_id)) {
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
ScanJob::rescan_entity(JobContext &context, const std::string &entity_id) {
  AiPreflight ai;
  return rescan_entity(context, entity_id, ai);
}

Result<ScanResult, JobError>
ScanJob::rescan_entity(JobContext &context, const std::string &entity_id,
                       AiPreflight &ai) {
  ScanJob job(entity_id);
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

  // 拿到响应头后才能判断内容类型和长度。
  const auto started = (*stream)->wait_started();
  if (!started) {
    if (started.error() == ByteStreamError::HttpError &&
        (*stream)->http_status() == 404) {
      return JobError::EntityNotFound;
    }
    return map_stream_error(started.error());
  }

  const std::string mime_type = media_type((*stream)->content_type());
  if (!mime_type.empty() && !is_image_mime(mime_type)) {
    return JobError::NotImage;
  }
  const auto content_length = (*stream)->content_length();
  if (content_length && *content_length > WdTagger::kMaxImageBytes) {
    return JobError::ImageTooLarge;
  }

  auto file = TempFile::create(context.work_dir, ".img");
  if (!file) {
    return JobError::FileError;
  }

  std::array<std::byte, kReadBufferSize> buffer{};
  while (true) {
    auto chunk = (*stream)->read(buffer);
    if (!chunk) {
      return map_stream_error(chunk.error());
    }
    if (*chunk == 0) {
      break;
    }
    auto written =
        file->write(std::span<const std::byte>(buffer.data(), *chunk));
    if (!written) {
      return JobError::FileError;
    }
  }

  auto closed = file->close();
  if (!closed) {
    return JobError::FileError;
  }

  auto tags = context.tagger.get().tag_file(file->path());
  if (!tags) {
    return map_tagger_error(tags.error());
  }

  // 复用同一个临时文件生成向量，不重复下载。
  auto embedding = context.embedder.get().embed_file(file->path());
  if (!embedding) {
    // CLIP 向量是入库前置条件，缺失直接让整单失败。
    return map_embedder_error(embedding.error());
  }

  ScanResult result;
  result.entity_id = entity_id_;
  result.bytes = file->size();
  result.tags = std::move(*tags);
  if (embedding) {
    result.embedding = std::move(embedding->values);
  }
  return result;
}

} // namespace tagimg
