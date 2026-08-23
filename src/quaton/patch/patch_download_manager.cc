#include "quaton/patch/patch_download_manager.h"

#include "quaton/http_client.h"
#include "quaton/logger.h"
#include "quaton/patch.h"
#include "quaton/patch/patch_download_job.h"

QUATON_NAMESPACE_BEGIN

PatchDownloadManager::PatchDownloadManager(
    std::shared_ptr<HttpClient> http_client)
    : DownloadManagerBase(), http_client_(http_client) {
}

PatchDownloadManager::~PatchDownloadManager() {
  Shutdown();
}

bool PatchDownloadManager::Initialize(const PatchManagerConfig& config) {
  patch_config_ = config;

  // Initialize base manager with inherited config fields
  if (!DownloadManagerBase::Initialize(config)) {
    LOG_ERROR("[PatchDownloadManager] Base initialization failed");
    return false;
  }

  LOG_INFO("[PatchDownloadManager] Initialized: {} -> {}",
           patch_config_.source_version,
           patch_config_.target_version);
  return true;
}

std::string PatchDownloadManager::AddPatches(
    const std::vector<PatchInfo>& patches) {
  std::vector<std::string> job_ids;
  job_ids.reserve(patches.size());

  for (const auto& patch : patches) {
    auto job_id = AddPatch(patch);
    if (!job_id.empty()) {
      job_ids.push_back(job_id);
    }
  }

  return job_ids.empty() ? "" : job_ids.front();
}

std::string PatchDownloadManager::AddPatch(const PatchInfo& patch) {
  auto item = CreateItemFromPatch(patch);
  if (!item) {
    LOG_ERROR("[PatchDownloadManager] Failed to create item for: {}",
              patch.patch_name);
    return "";
  }

  {
    std::lock_guard<std::mutex> lock(items_mutex_);
    items_[patch.patch_name] = item;
  }

  item->TransitionTo(DownloadState::kPending);

  LOG_DEBUG("[PatchDownloadManager] Added patch: {} ({} bytes, {})",
            patch.patch_name,
            patch.patch_size,
            item->GetPatchTypeString());

  return patch.patch_name;
}

std::shared_ptr<PatchDownloadItem> PatchDownloadManager::CreateItemFromPatch(
    const PatchInfo& patch) {
  auto item = std::make_shared<PatchDownloadItem>(
      patch.patch_name, patch.patch_size, patch.patch_hash);

  item->SetPatchType(patch_config_.default_patch_type);
  item->SetSourceFilePath(patch_config_.source_path + "/" + patch.source_file);
  item->SetTargetFilePath(patch_config_.target_path + "/" + patch.target_file);
  item->SetTargetHash(patch.target_hash);
  item->SetTargetSize(patch.target_size);
  item->SetSourceVersion(patch_config_.source_version);
  item->SetTargetVersion(patch_config_.target_version);

  std::string dest_path = patch_config_.download_path + "/" + patch.patch_name;
  item->SetFilePath(dest_path);

  return item;
}

size_t PatchDownloadManager::StartAllPatches() {
  std::lock_guard<std::mutex> lock(items_mutex_);
  size_t started = 0;

  for (auto& [patch_name, item] : items_) {
    if (item->GetState() == DownloadState::kPending) {
      auto job_id = CreatePatchJob(item);
      if (!job_id.empty()) {
        job_to_patch_map_[job_id] = patch_name;
        started++;
      }
    }
  }

  LOG_INFO("[PatchDownloadManager] Started {} patch jobs", started);
  return started;
}

void PatchDownloadManager::PauseAllPatches() {
  std::lock_guard<std::mutex> lock(items_mutex_);

  for (auto& [patch_name, item] : items_) {
    auto state = item->GetState();
    if (state == DownloadState::kDownloading ||
        state == DownloadState::kApplying) {
      item->TransitionTo(DownloadState::kPaused);
    }
  }

  LOG_INFO("[PatchDownloadManager] Paused all patches");
}

void PatchDownloadManager::ResumeAllPatches() {
  std::lock_guard<std::mutex> lock(items_mutex_);

  for (auto& [patch_name, item] : items_) {
    if (item->GetState() == DownloadState::kPaused) {
      item->TransitionTo(DownloadState::kPending);
      CreatePatchJob(item);
    }
  }

  LOG_INFO("[PatchDownloadManager] Resumed paused patches");
}

PatchDownloadManager::PatchStats PatchDownloadManager::GetPatchStats() const {
  std::lock_guard<std::mutex> lock(items_mutex_);
  PatchStats stats;

  double total_apply_progress = 0.0;

  for (const auto& [patch_name, item] : items_) {
    stats.total_patches++;
    stats.total_patch_bytes += item->GetTotalSize();
    stats.downloaded_bytes += item->GetDownloadedSize();

    switch (item->GetState()) {
      case DownloadState::kCompleted:
        stats.applied++;
        stats.downloaded++;
        total_apply_progress += 1.0;
        break;
      case DownloadState::kApplying:
        stats.downloaded++;
        total_apply_progress += item->GetApplyProgress();
        break;
      case DownloadState::kVerifying:
        stats.downloaded++;
        total_apply_progress += item->GetApplyProgress();
        break;
      case DownloadState::kFailed:
        stats.failed++;
        break;
      case DownloadState::kPending:
        stats.pending++;
        break;
      case DownloadState::kDownloading:
        stats.active++;
        break;
      default:
        break;
    }
  }

  if (stats.total_patches > 0) {
    stats.apply_progress = total_apply_progress / stats.total_patches;
  }

  return stats;
}

std::shared_ptr<PatchDownloadItem> PatchDownloadManager::GetItemByPatchName(
    const std::string& patch_name) const {
  std::lock_guard<std::mutex> lock(items_mutex_);
  auto it = items_.find(patch_name);
  return it != items_.end() ? it->second : nullptr;
}

std::vector<std::shared_ptr<PatchDownloadItem>>
PatchDownloadManager::GetAllItems() const {
  std::lock_guard<std::mutex> lock(items_mutex_);
  std::vector<std::shared_ptr<PatchDownloadItem>> result;
  result.reserve(items_.size());
  for (const auto& [name, item] : items_) {
    result.push_back(item);
  }
  return result;
}

std::vector<std::shared_ptr<PatchDownloadItem>>
PatchDownloadManager::GetItemsByState(DownloadState state) const {
  std::lock_guard<std::mutex> lock(items_mutex_);
  std::vector<std::shared_ptr<PatchDownloadItem>> result;
  for (const auto& [name, item] : items_) {
    if (item->GetState() == state) {
      result.push_back(item);
    }
  }
  return result;
}

std::string PatchDownloadManager::CreatePatchJob(
    std::shared_ptr<PatchDownloadItem> item) {
  std::string job_id = GenerateJobId("patch");

  auto job = std::make_shared<PatchDownloadJob>(job_id,
                                                http_client_,
                                                patch_config_.target_path,
                                                patch_config_.download_path);

  job->SetPatchBaseUrl(patch_config_.patch_base_url);
  job->SetApplyEnabled(true);
  job->SetCleanupEnabled(patch_config_.delete_patch_after_apply);
  job->SetVerifyEnabled(patch_config_.verify_after_download);
  job->SetMaxRetries(patch_config_.max_retry_count);

  // Use the typed method so the item is also tracked in patch_items_
  job->AddPatchItem(item);

  auto result_id = SubmitJob(job, TaskPriority::kNormal);

  if (!result_id.empty()) {
    item->TransitionTo(DownloadState::kDownloading);
    LOG_DEBUG("[PatchDownloadManager] Created job {} for patch: {}",
              result_id,
              item->GetFileName());
  }

  return result_id;
}

bool PatchDownloadManager::ApplyPatch(std::shared_ptr<PatchDownloadItem> item) {
  item->TransitionTo(DownloadState::kApplying);

  // TODO: Implement actual patch application using LDIFF/HPatch
  // For now, simulate the process

  LOG_INFO("[PatchDownloadManager] Applying patch: {} -> {}",
           item->GetSourceFilePath(),
           item->GetTargetFilePath());

  // Simulate progress
  for (int i = 0; i <= 100; i += 10) {
    item->SetApplyProgress(i / 100.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // Verify if enabled
  if (config_.verify_after_download) {
    item->TransitionTo(DownloadState::kVerifying);
    // TODO: Verify target hash
    LOG_DEBUG("[PatchDownloadManager] Verifying: {}",
              item->GetTargetFilePath());
  }

  item->SetApplied(true);
  item->TransitionTo(DownloadState::kCompleted);

  // Delete patch file if configured
  if (patch_config_.delete_patch_after_apply) {
    // TODO: Delete patch file
  }

  return true;
}

void PatchDownloadManager::OnJobStateChanged(const std::string& job_id,
                                             JobState old_state,
                                             JobState new_state) {
  LOG_DEBUG("[PatchDownloadManager] Job {} state: {} -> {}",
            job_id,
            static_cast<int>(old_state),
            static_cast<int>(new_state));
}

void PatchDownloadManager::OnJobProgress(const std::string& job_id,
                                         int64_t downloaded,
                                         int64_t total) {
  auto it = job_to_patch_map_.find(job_id);
  if (it != job_to_patch_map_.end()) {
    auto item = GetItemByPatchName(it->second);
    if (item) {
      item->UpdateProgress(downloaded, total);
    }
  }
}

void PatchDownloadManager::OnJobCompleted(const std::string& job_id) {
  auto it = job_to_patch_map_.find(job_id);
  if (it != job_to_patch_map_.end()) {
    auto item = GetItemByPatchName(it->second);
    if (item) {
      // Download complete, now apply patch
      ApplyPatch(item);
    }
    job_to_patch_map_.erase(it);
  }
}

void PatchDownloadManager::OnJobFailed(const std::string& job_id,
                                       const std::string& error) {
  auto it = job_to_patch_map_.find(job_id);
  if (it != job_to_patch_map_.end()) {
    auto item = GetItemByPatchName(it->second);
    if (item) {
      item->SetError(-1, error, "", true);
      item->TransitionTo(DownloadState::kFailed);
    }
    job_to_patch_map_.erase(it);
  }
}

QUATON_NAMESPACE_END
