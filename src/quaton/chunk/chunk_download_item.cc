#include "quaton/chunk/chunk_download_item.h"

#include "quaton/logger.h"

QUATON_NAMESPACE_BEGIN

ChunkDownloadItem::ChunkDownloadItem(const std::string& file_name,
                                     int64_t file_size,
                                     const std::string& file_hash)
    : DownloadItem(file_name) {
  SetExpectedSize(file_size);
  SetExpectedChecksum(file_hash);
  SetFilePath(file_name);
}

void ChunkDownloadItem::OnStateChanged(DownloadState old_state,
                                       DownloadState new_state) {
  LOG_DEBUG("[ChunkDownloadItem] File '{}' state: {} -> {}",
            GetId(),
            static_cast<int>(old_state),
            static_cast<int>(new_state));

  // Handle chunk-specific state transitions
  switch (new_state) {
    case DownloadState::kDownloading:
      // Could initialize chunk download tracking here
      break;

    case DownloadState::kVerifying:
      // Verification will compare calculated_checksum_ with expected
      break;

    case DownloadState::kCompleted:
      LOG_INFO("[ChunkDownloadItem] File '{}' download completed", GetId());
      break;

    case DownloadState::kFailed: {
      auto error = GetError();
      LOG_ERROR("[ChunkDownloadItem] File '{}' failed: [{}] {}",
                GetId(),
                error.error_code,
                error.error_message);
      break;
    }

    default:
      break;
  }
}

QUATON_NAMESPACE_END
