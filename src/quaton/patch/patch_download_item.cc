#include "quaton/patch/patch_download_item.h"

#include "quaton/logger.h"

QUATON_NAMESPACE_BEGIN

PatchDownloadItem::PatchDownloadItem(const std::string& patch_name,
                                     int64_t patch_size,
                                     const std::string& patch_hash)
    : DownloadItem(patch_name) {
  SetExpectedSize(patch_size);
  SetExpectedChecksum(patch_hash);
  SetFilePath(patch_name);
}

std::string PatchDownloadItem::GetPatchTypeString() const {
  switch (patch_type_) {
    case PatchType::kLdiff:
      return "ldiff";
    case PatchType::kHPatch:
      return "hpatch";
    case PatchType::kBsDiff:
      return "bsdiff";
    case PatchType::kXDelta:
      return "xdelta";
    default:
      return "unknown";
  }
}

void PatchDownloadItem::OnStateChanged(DownloadState old_state,
                                       DownloadState new_state) {
  LOG_DEBUG("[PatchDownloadItem] Patch '{}' state: {} -> {}",
            GetId(),
            static_cast<int>(old_state),
            static_cast<int>(new_state));

  switch (new_state) {
    case DownloadState::kDownloading:
      LOG_INFO("[PatchDownloadItem] Downloading patch: {} ({})",
               GetId(),
               GetPatchTypeString());
      break;

    case DownloadState::kApplying:
      LOG_INFO("[PatchDownloadItem] Applying patch: {} to {}",
               GetId(),
               source_file_path_);
      break;

    case DownloadState::kVerifying:
      LOG_DEBUG("[PatchDownloadItem] Verifying patched file: {}",
                target_file_path_);
      break;

    case DownloadState::kCompleted:
      is_applied_ = true;
      LOG_INFO("[PatchDownloadItem] Patch '{}' completed successfully",
               GetId());
      break;

    case DownloadState::kFailed: {
      auto error = GetError();
      LOG_ERROR("[PatchDownloadItem] Patch '{}' failed: [{}] {}",
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
