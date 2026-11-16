#include "job/job.hpp"

namespace tagimg {

std::string_view to_string(JobError error) noexcept {
  switch (error) {
  case JobError::InvalidArgument:
    return "InvalidArgument";
  case JobError::InvalidEntityId:
    return "InvalidEntityId";
  case JobError::EntityNotFound:
    return "EntityNotFound";
  case JobError::NotImage:
    return "NotImage";
  case JobError::RepositoryFailed:
    return "RepositoryFailed";
  case JobError::DownloadFailed:
    return "DownloadFailed";
  case JobError::FileError:
    return "FileError";
  case JobError::ImageTooLarge:
    return "ImageTooLarge";
  case JobError::TaggerUnavailable:
    return "TaggerUnavailable";
  case JobError::TaggingFailed:
    return "TaggingFailed";
  case JobError::EmbeddingFailed:
    return "EmbeddingFailed";
  case JobError::DuplicateImage:
    return "DuplicateImage";
  case JobError::UnknownTag:
    return "UnknownTag";
  case JobError::UnknownArtist:
    return "UnknownArtist";
  case JobError::TagHierarchyMissing:
    return "TagHierarchyMissing";
  case JobError::WriteFailed:
    return "WriteFailed";
  case JobError::InternalError:
    return "InternalError";
  case JobError::JobNotFinished:
    return "JobNotFinished";
  }
  return "Unknown";
}

} // namespace tagimg
