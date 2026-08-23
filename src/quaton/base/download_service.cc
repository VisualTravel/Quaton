#include "quaton/base/download_service.h"

#include <thread>

#include "quaton/logger.h"

namespace Quaton {

DownloadService::DownloadService(const ServiceConfig& config)
    : config_(config) {
  if (config_.thread_count <= 0) {
    config_.thread_count =
        static_cast<int>(std::thread::hardware_concurrency());
    if (config_.thread_count <= 0) {
      config_.thread_count = 4;
    }
  }
}

DownloadService::~DownloadService() {
  Shutdown();
}

bool DownloadService::Initialize() {
  if (initialized_.load()) {
    LOG_WARN("Service already initialized");
    return true;
  }

  try {
    http_client_ = CreateHttpClient();

    SchedulerConfig sched_config;
    sched_config.max_concurrent_jobs = config_.thread_count / 2;
    if (sched_config.max_concurrent_jobs < 1) {
      sched_config.max_concurrent_jobs = 1;
    }
    sched_config.max_concurrent_items = config_.thread_count;
    sched_config.max_retries = config_.retry_count;

    scheduler_ = std::make_unique<DownloadScheduler>(sched_config);
    scheduler_->Start();

    scheduler_->SetProgressCallback(
        [this](size_t total, size_t completed, size_t failed) {
          std::lock_guard<std::mutex> lock(mutex_);
          statistics_.total_items = total;
          statistics_.completed_items = completed;
          statistics_.failed_items = failed;
          ReportProgress();
        });

    initialized_.store(true);
    start_time_ = std::chrono::steady_clock::now();

    LOG_INFO("DownloadService initialized with %d threads",
             config_.thread_count);
    return true;

  } catch (const std::exception& e) {
    LOG_ERROR("Failed to initialize service: %s", e.what());
    return false;
  }
}

void DownloadService::Shutdown() {
  if (!initialized_.load()) {
    return;
  }

  LOG_INFO("Shutting down DownloadService...");

  if (scheduler_) {
    scheduler_->Stop(true);
    scheduler_.reset();
  }

  http_client_.reset();

  initialized_.store(false);

  LOG_INFO("DownloadService shutdown complete");
}

bool DownloadService::IsBusy() const {
  if (!scheduler_) {
    return false;
  }
  return scheduler_->GetRunningJobCount() > 0 ||
         scheduler_->GetPendingJobCount() > 0;
}

void DownloadService::CancelAll() {
  if (scheduler_) {
    scheduler_->CancelAll();
  }
}

void DownloadService::PauseAll() {
  if (scheduler_) {
    scheduler_->PauseAll();
  }
}

void DownloadService::ResumeAll() {
  if (scheduler_) {
    scheduler_->ResumeAll();
  }
}

void DownloadService::SetProgressCallback(ProgressCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  progress_callback_ = std::move(callback);
}

ServiceStatistics DownloadService::GetStatistics() const {
  std::lock_guard<std::mutex> lock(mutex_);

  ServiceStatistics stats = statistics_;

  auto now = std::chrono::steady_clock::now();
  stats.elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_)
          .count();

  return stats;
}

void DownloadService::UpdateConfig(const ServiceConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_ = config;

  if (scheduler_) {
    SchedulerConfig sched_config;
    sched_config.max_concurrent_jobs = config_.thread_count / 2;
    if (sched_config.max_concurrent_jobs < 1) {
      sched_config.max_concurrent_jobs = 1;
    }
    sched_config.max_concurrent_items = config_.thread_count;
    sched_config.max_retries = config_.retry_count;
    scheduler_->UpdateConfig(sched_config);
  }
}

std::shared_ptr<HttpClient> DownloadService::CreateHttpClient() {
  HttpConfig http_config;
  http_config.timeout_ms = config_.request_timeout_ms;
  http_config.max_connections_per_server = config_.max_http_handles;
  return std::make_shared<HttpClient>(http_config);
}

void DownloadService::UpdateStatistics(const ServiceStatistics& delta) {
  std::lock_guard<std::mutex> lock(mutex_);
  statistics_.total_jobs += delta.total_jobs;
  statistics_.completed_jobs += delta.completed_jobs;
  statistics_.failed_jobs += delta.failed_jobs;
  statistics_.total_items += delta.total_items;
  statistics_.completed_items += delta.completed_items;
  statistics_.failed_items += delta.failed_items;
  statistics_.skipped_items += delta.skipped_items;
  statistics_.total_bytes += delta.total_bytes;
  statistics_.downloaded_bytes += delta.downloaded_bytes;
}

void DownloadService::ReportProgress() {
  // Must be called with mutex held or from a thread-safe context
  if (!progress_callback_) {
    return;
  }

  ProgressInfo info;
  info.total_files = statistics_.total_items;
  info.completed_files = statistics_.completed_items;
  info.failed_files = statistics_.failed_items;

  if (statistics_.total_bytes > 0) {
    info.overall_percentage =
        (static_cast<double>(statistics_.downloaded_bytes) /
         statistics_.total_bytes) *
        100.0;
  } else if (statistics_.total_items > 0) {
    info.overall_percentage =
        (static_cast<double>(statistics_.completed_items) /
         statistics_.total_items) *
        100.0;
  }

  info.download_speed = statistics_.current_speed / (1024.0 * 1024.0);  // MB/s

  if (statistics_.current_speed > 0 &&
      statistics_.downloaded_bytes < statistics_.total_bytes) {
    int64_t remaining_bytes =
        statistics_.total_bytes - statistics_.downloaded_bytes;
    int64_t eta_seconds =
        static_cast<int64_t>(remaining_bytes / statistics_.current_speed);
    info.estimated_minutes = static_cast<int>(eta_seconds / 60);
    info.estimated_remaining_seconds = static_cast<int>(eta_seconds % 60);
  }

  progress_callback_(info);
}

}  // namespace Quaton
