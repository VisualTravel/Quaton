#include "quaton/chunk/chunk_download_manager.h"

#include "quaton/chunk/chunk_download_job.h"
#include "quaton/http_client.h"
#include "quaton/logger.h"

QUATON_NAMESPACE_BEGIN

ChunkDownloadManager::ChunkDownloadManager(
    std::shared_ptr<HttpClient> http_client)
    : DownloadManagerBase(), http_client_(http_client) {
}

ChunkDownloadManager::~ChunkDownloadManager() {
  Shutdown();
}

bool ChunkDownloadManager::Initialize(const ChunkManagerConfig& config) {
  chunk_config_ = config;

  // Initialize base manager with inherited config fields
  if (!DownloadManagerBase::Initialize(config)) {
    LOG_ERROR("[ChunkDownloadManager] Base initialization failed");
    return false;
  }

  LOG_INFO("[ChunkDownloadManager] Initialized with base_url: {}",
           chunk_config_.chunk_base_url);
  return true;
}

std::string ChunkDownloadManager::AddResources(
    const std::vector<Resource>& resources) {
  std::vector<std::string> job_ids;
  job_ids.reserve(resources.size());

  for (const auto& resource : resources) {
    auto job_id = AddResource(resource);
    if (!job_id.empty()) {
      job_ids.push_back(job_id);
    }
  }

  // Return first job ID as batch identifier
  return job_ids.empty() ? "" : job_ids.front();
}

std::string ChunkDownloadManager::AddResource(const Resource& resource) {
  auto item = CreateItemFromResource(resource);
  if (!item) {
    LOG_ERROR("[ChunkDownloadManager] Failed to create item for: {}",
              resource.name_);
    return "";
  }

  {
    std::lock_guard<std::mutex> lock(items_mutex_);
    items_[resource.name_] = item;
  }

  item->TransitionTo(DownloadState::kPending);

  LOG_DEBUG("[ChunkDownloadManager] Added resource: {} ({} bytes)",
            resource.name_,
            resource.size_);

  return resource.name_;  // Use file name as initial identifier
}

std::shared_ptr<ChunkDownloadItem> ChunkDownloadManager::CreateItemFromResource(
    const Resource& resource) {
  auto item = std::make_shared<ChunkDownloadItem>(
      resource.name_, resource.size_, resource.hash_);

  item->SetChunkBaseUrl(chunk_config_.chunk_base_url);
  item->SetPackageId(chunk_config_.package_id);
  item->SetBuildId(chunk_config_.build_id);
  item->SetVersion(chunk_config_.version);
  item->SetCompressed(true);

  for (const auto& chunk : resource.chunks_) {
    item->AddChunk(chunk);
  }

  std::string dest_path = chunk_config_.download_path + "/" + resource.name_;
  item->SetDestinationPath(dest_path);

  return item;
}

size_t ChunkDownloadManager::StartAllDownloads() {
  std::lock_guard<std::mutex> lock(items_mutex_);
  size_t started = 0;

  for (auto& [file_name, item] : items_) {
    if (item->GetState() == DownloadState::kPending) {
      auto job_id = CreateChunkJob(item);
      if (!job_id.empty()) {
        job_to_file_map_[job_id] = file_name;
        started++;
      }
    }
  }

  LOG_INFO("[ChunkDownloadManager] Started {} downloads", started);
  return started;
}

void ChunkDownloadManager::PauseAllDownloads() {
  std::lock_guard<std::mutex> lock(items_mutex_);

  for (auto& [file_name, item] : items_) {
    if (item->GetState() == DownloadState::kDownloading) {
      item->TransitionTo(DownloadState::kPaused);
      for (auto& [job_id, fname] : job_to_file_map_) {
        if (fname == file_name) {
          CancelJob(job_id);
          break;
        }
      }
    }
  }

  LOG_INFO("[ChunkDownloadManager] Paused all downloads");
}

void ChunkDownloadManager::ResumeAllDownloads() {
  std::lock_guard<std::mutex> lock(items_mutex_);

  for (auto& [file_name, item] : items_) {
    if (item->GetState() == DownloadState::kPaused) {
      item->TransitionTo(DownloadState::kPending);
      CreateChunkJob(item);
    }
  }

  LOG_INFO("[ChunkDownloadManager] Resumed paused downloads");
}

ChunkDownloadManager::ChunkDownloadStats
ChunkDownloadManager::GetDownloadStats() const {
  std::lock_guard<std::mutex> lock(items_mutex_);
  ChunkDownloadStats stats;

  for (const auto& [file_name, item] : items_) {
    stats.total_files++;
    stats.total_bytes += item->GetTotalSize();
    stats.downloaded_bytes += item->GetDownloadedSize();

    switch (item->GetState()) {
      case DownloadState::kCompleted:
        stats.completed_files++;
        break;
      case DownloadState::kFailed:
        stats.failed_files++;
        break;
      case DownloadState::kPending:
        stats.pending_files++;
        break;
      case DownloadState::kDownloading:
        stats.active_files++;
        break;
      default:
        break;
    }
  }

  return stats;
}

std::shared_ptr<ChunkDownloadItem> ChunkDownloadManager::GetItemByFileName(
    const std::string& file_name) const {
  std::lock_guard<std::mutex> lock(items_mutex_);
  auto it = items_.find(file_name);
  return it != items_.end() ? it->second : nullptr;
}

std::vector<std::shared_ptr<ChunkDownloadItem>>
ChunkDownloadManager::GetAllItems() const {
  std::lock_guard<std::mutex> lock(items_mutex_);
  std::vector<std::shared_ptr<ChunkDownloadItem>> result;
  result.reserve(items_.size());
  for (const auto& [file_name, item] : items_) {
    result.push_back(item);
  }
  return result;
}

std::vector<std::shared_ptr<ChunkDownloadItem>>
ChunkDownloadManager::GetItemsByState(DownloadState state) const {
  std::lock_guard<std::mutex> lock(items_mutex_);
  std::vector<std::shared_ptr<ChunkDownloadItem>> result;
  for (const auto& [file_name, item] : items_) {
    if (item->GetState() == state) {
      result.push_back(item);
    }
  }
  return result;
}

std::string ChunkDownloadManager::CreateChunkJob(
    std::shared_ptr<ChunkDownloadItem> item) {
  std::string job_id = GenerateJobId("chunk");

  auto job = std::make_shared<ChunkDownloadJob>(
      job_id, http_client_, chunk_config_.download_path);

  job->SetChunkBaseUrl(chunk_config_.chunk_base_url);
  job->SetVerifyEnabled(chunk_config_.verify_after_download);
  job->SetDecompressEnabled(chunk_config_.decompress_chunks);
  job->SetMaxRetries(chunk_config_.max_retry_count);

  // Use the typed method so the item is also tracked in chunk_items_
  job->AddChunkItem(item);

  std::string result_id = SubmitJob(job, TaskPriority::kNormal);

  if (!result_id.empty()) {
    item->TransitionTo(DownloadState::kDownloading);
    LOG_DEBUG("[ChunkDownloadManager] Created job {} for file: {}",
              result_id,
              item->GetFileName());
  }

  return result_id;
}

void ChunkDownloadManager::OnJobStateChanged(const std::string& job_id,
                                             JobState old_state,
                                             JobState new_state) {
  LOG_DEBUG("[ChunkDownloadManager] Job {} state: {} -> {}",
            job_id,
            static_cast<int>(old_state),
            static_cast<int>(new_state));
}

void ChunkDownloadManager::OnJobProgress(const std::string& job_id,
                                         int64_t downloaded,
                                         int64_t total) {
  auto it = job_to_file_map_.find(job_id);
  if (it != job_to_file_map_.end()) {
    auto item = GetItemByFileName(it->second);
    if (item) {
      item->UpdateProgress(downloaded, total);
    }
  }
}

void ChunkDownloadManager::OnJobCompleted(const std::string& job_id) {
  auto it = job_to_file_map_.find(job_id);
  if (it != job_to_file_map_.end()) {
    auto item = GetItemByFileName(it->second);
    if (item) {
      if (config_.verify_after_download) {
        item->TransitionTo(DownloadState::kVerifying);
        // TODO: Trigger verification
        // For now, assume success
        item->TransitionTo(DownloadState::kCompleted);
      } else {
        item->TransitionTo(DownloadState::kCompleted);
      }
    }
    job_to_file_map_.erase(it);
  }
}

void ChunkDownloadManager::OnJobFailed(const std::string& job_id,
                                       const std::string& error) {
  auto it = job_to_file_map_.find(job_id);
  if (it != job_to_file_map_.end()) {
    auto item = GetItemByFileName(it->second);
    if (item) {
      item->SetError(-1, error, "", true);
      item->TransitionTo(DownloadState::kFailed);
    }
    job_to_file_map_.erase(it);
  }
}

QUATON_NAMESPACE_END
