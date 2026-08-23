#include "quaton/base/download_job.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "quaton/database/database_manager.h"
#include "quaton/downloader/downloader_utils.h"
#include "quaton/logger.h"
#include "quaton/utils/checksum.h"

namespace fs = std::filesystem;

namespace Quaton {

// ============================================================================
// DownloadJob Implementation
// ============================================================================

DownloadJob::DownloadJob(const std::string& job_id,
                         std::shared_ptr<HttpClient> http_client)
    : job_id_(job_id), http_client_(std::move(http_client)) {
}

bool DownloadJob::Start() {
  if (!TransitionTo(JobState::kRunning)) {
    LOG_ERROR("Job %s: Failed to start, invalid state", job_id_.c_str());
    return false;
  }

  LOG_INFO("Job %s: Started with %zu items", job_id_.c_str(), items_.size());

  // Process items in background
  std::thread([this]() {
    for (auto& item : items_) {
      if (cancelled_.load()) {
        item->TransitionTo(DownloadState::kCancelled, "Job cancelled");
        continue;
      }

      // Transition item to pending
      item->TransitionTo(DownloadState::kPending, "Job started");

      // Process the item
      bool success = ProcessItem(item);

      if (success) {
        OnItemCompleted(item);
      } else {
        OnItemFailed(item);
      }
    }

    OnAllItemsProcessed();
  }).detach();

  return true;
}

void DownloadJob::Pause() {
  TransitionTo(JobState::kSuspended);
  LOG_INFO("Job %s: Paused", job_id_.c_str());
}

void DownloadJob::Resume() {
  if (state_.load() == JobState::kSuspended) {
    TransitionTo(JobState::kRunning);
    LOG_INFO("Job %s: Resumed", job_id_.c_str());
  }
}

void DownloadJob::Cancel() {
  cancelled_.store(true);
  TransitionTo(JobState::kCancelled);

  // Cancel all pending items
  for (auto& item : items_) {
    if (!item->IsTerminal()) {
      item->TransitionTo(DownloadState::kCancelled, "Job cancelled");
    }
  }

  // Notify completion
  completion_cv_.notify_all();

  LOG_INFO("Job %s: Cancelled", job_id_.c_str());
}

bool DownloadJob::WaitForCompletion(int timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);

  if (timeout_ms <= 0) {
    completion_cv_.wait(lock, [this]() { return IsComplete(); });
    return true;
  }

  return completion_cv_.wait_for(lock,
                                 std::chrono::milliseconds(timeout_ms),
                                 [this]() { return IsComplete(); });
}

void DownloadJob::AddItem(std::shared_ptr<DownloadItem> item) {
  std::lock_guard<std::mutex> lock(mutex_);
  items_.push_back(std::move(item));
}

bool DownloadJob::TransitionTo(JobState new_state) {
  JobState old_state = state_.load();
  state_.store(new_state);
  LOG_DEBUG("Job %s: %s -> %s",
            job_id_.c_str(),
            JobStateToString(old_state),
            JobStateToString(new_state));
  return true;
}

void DownloadJob::OnAllItemsProcessed() {
  bool all_success = (failed_count_.load() == 0);

  if (cancelled_.load()) {
    TransitionTo(JobState::kCancelled);
  } else if (all_success) {
    TransitionTo(JobState::kCompleted);
  } else {
    TransitionTo(JobState::kFailed);
  }

  // Notify callbacks
  if (completion_callback_) {
    std::string error_msg =
        all_success
            ? ""
            : "Some items failed: " + std::to_string(failed_count_.load());
    completion_callback_(all_success, error_msg);
  }

  completion_cv_.notify_all();

  LOG_INFO("Job %s: Completed. Success: %zu, Failed: %zu",
           job_id_.c_str(),
           completed_count_.load(),
           failed_count_.load());
}

void DownloadJob::OnItemCompleted(std::shared_ptr<DownloadItem> item) {
  completed_count_++;
  item->TransitionTo(DownloadState::kCompleted, "Download succeeded");

  ReportProgress(
      completed_count_.load() + failed_count_.load(), items_.size(), 0);
}

void DownloadJob::OnItemFailed(std::shared_ptr<DownloadItem> item) {
  if (item->CanRetry() && item->GetRetryCount() < max_retries_) {
    item->IncrementRetryCount();
    item->TransitionTo(DownloadState::kPending, "Retrying");

    LOG_INFO("Job %s: Retrying item %s (attempt %d/%d)",
             job_id_.c_str(),
             item->GetId().c_str(),
             item->GetRetryCount(),
             max_retries_);

    if (ProcessItem(item)) {
      OnItemCompleted(item);
      return;
    }
  }

  failed_count_++;
  item->TransitionTo(DownloadState::kFailed, "Download failed");

  LOG_ERROR("Job %s: Item %s failed after %d retries",
            job_id_.c_str(),
            item->GetId().c_str(),
            item->GetRetryCount());

  // Report progress
  ReportProgress(
      completed_count_.load() + failed_count_.load(), items_.size(), 0);
}

void DownloadJob::ReportProgress(int64_t processed,
                                 int64_t total,
                                 double speed) {
  if (progress_callback_) {
    progress_callback_(processed, total, speed);
  }
}

}  // namespace Quaton
