#include "quaton/base/download_scheduler.h"

#include <algorithm>

#include "quaton/logger.h"

namespace Quaton {

DownloadScheduler::DownloadScheduler(const SchedulerConfig& config)
    : config_(config) {
}

DownloadScheduler::~DownloadScheduler() {
  Stop(false);
}

void DownloadScheduler::Start() {
  if (running_.load()) {
    LOG_WARN("Scheduler already running");
    return;
  }

  running_.store(true);
  paused_.store(false);

  scheduler_thread_ = std::thread(&DownloadScheduler::SchedulerLoop, this);

  LOG_INFO("Scheduler started with max %d concurrent jobs",
           config_.max_concurrent_jobs);
}

void DownloadScheduler::Stop(bool wait_for_completion) {
  if (!running_.load()) {
    return;
  }

  if (wait_for_completion) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this]() { return running_jobs_.empty(); });
  }

  running_.store(false);
  cv_.notify_all();

  if (scheduler_thread_.joinable()) {
    scheduler_thread_.join();
  }

  LOG_INFO("Scheduler stopped");
}

void DownloadScheduler::PauseAll() {
  paused_.store(true);

  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [id, job] : running_jobs_) {
    job->Pause();
  }

  LOG_INFO("Scheduler paused, %zu jobs paused", running_jobs_.size());
}

void DownloadScheduler::ResumeAll() {
  paused_.store(false);

  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [id, job] : running_jobs_) {
    job->Resume();
  }

  cv_.notify_all();

  LOG_INFO("Scheduler resumed");
}

void DownloadScheduler::CancelAll() {
  std::lock_guard<std::mutex> lock(mutex_);

  for (auto& [id, job] : running_jobs_) {
    job->Cancel();
  }

  while (!pending_jobs_.empty()) {
    auto job = pending_jobs_.top();
    pending_jobs_.pop();
    job->Cancel();
  }

  running_jobs_.clear();

  LOG_INFO("Scheduler: All jobs cancelled");
}

bool DownloadScheduler::SubmitJob(std::shared_ptr<DownloadJob> job) {
  if (!running_.load()) {
    LOG_ERROR("Scheduler not running, cannot submit job");
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    all_jobs_[job->GetJobId()] = job;
    pending_jobs_.push(job);
  }

  cv_.notify_one();

  LOG_INFO("Scheduler: Job %s submitted (%zu items)",
           job->GetJobId().c_str(),
           job->GetItemCount());

  return true;
}

bool DownloadScheduler::CancelJob(const std::string& job_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = all_jobs_.find(job_id);
  if (it == all_jobs_.end()) {
    LOG_WARN("Scheduler: Job %s not found", job_id.c_str());
    return false;
  }

  it->second->Cancel();

  running_jobs_.erase(job_id);

  LOG_INFO("Scheduler: Job %s cancelled", job_id.c_str());
  return true;
}

std::shared_ptr<DownloadJob> DownloadScheduler::GetJob(
    const std::string& job_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = all_jobs_.find(job_id);
  return it != all_jobs_.end() ? it->second : nullptr;
}

std::vector<std::shared_ptr<DownloadJob>> DownloadScheduler::GetAllJobs()
    const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<DownloadJob>> result;
  result.reserve(all_jobs_.size());
  for (const auto& [id, job] : all_jobs_) {
    result.push_back(job);
  }
  return result;
}

size_t DownloadScheduler::GetPendingJobCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pending_jobs_.size();
}

size_t DownloadScheduler::GetRunningJobCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return running_jobs_.size();
}

void DownloadScheduler::GetAggregateProgress(size_t& total_items,
                                             size_t& completed_items,
                                             size_t& failed_items) const {
  std::lock_guard<std::mutex> lock(mutex_);

  total_items = 0;
  completed_items = 0;
  failed_items = 0;

  for (const auto& [id, job] : all_jobs_) {
    total_items += job->GetItemCount();
    completed_items += job->GetCompletedItemCount();
    failed_items += job->GetFailedItemCount();
  }
}

void DownloadScheduler::SetProgressCallback(
    std::function<void(size_t, size_t, size_t)> callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  progress_callback_ = std::move(callback);
}

void DownloadScheduler::UpdateConfig(const SchedulerConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_ = config;
  LOG_INFO("Scheduler config updated");
}

SchedulerConfig DownloadScheduler::GetConfig() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return config_;
}

void DownloadScheduler::SchedulerLoop() {
  LOG_DEBUG("Scheduler loop started");

  while (running_.load()) {
    std::unique_lock<std::mutex> lock(mutex_);

    // Wait for work or stop signal
    cv_.wait(lock, [this]() {
      return !running_.load() ||
             (!paused_.load() && CanStartMoreJobs() && !pending_jobs_.empty());
    });

    if (!running_.load()) {
      break;
    }

    if (paused_.load()) {
      continue;
    }

    ProcessNextJob();
  }

  LOG_DEBUG("Scheduler loop ended");
}

void DownloadScheduler::ProcessNextJob() {
  // Must be called with mutex held
  if (pending_jobs_.empty() || !CanStartMoreJobs()) {
    return;
  }

  auto job = pending_jobs_.top();
  pending_jobs_.pop();

  job->SetCompletionCallback([this, job](bool success, const std::string& msg) {
    OnJobCompleted(job, success);
  });

  running_jobs_[job->GetJobId()] = job;

  LOG_INFO("Scheduler: Starting job %s", job->GetJobId().c_str());
  job->Start();
}

void DownloadScheduler::OnJobCompleted(std::shared_ptr<DownloadJob> job,
                                       bool success) {
  {
    std::lock_guard<std::mutex> lock(mutex_);

    running_jobs_.erase(job->GetJobId());

    LOG_INFO("Scheduler: Job %s completed (success: %s)",
             job->GetJobId().c_str(),
             success ? "true" : "false");

    if (progress_callback_) {
      size_t total, completed, failed;
      // Call without lock to avoid deadlock
      total = 0;
      completed = 0;
      failed = 0;
      for (const auto& [id, j] : all_jobs_) {
        total += j->GetItemCount();
        completed += j->GetCompletedItemCount();
        failed += j->GetFailedItemCount();
      }
      progress_callback_(total, completed, failed);
    }
  }

  cv_.notify_one();
}

bool DownloadScheduler::CanStartMoreJobs() const {
  // Must be called with mutex held
  return static_cast<int>(running_jobs_.size()) < config_.max_concurrent_jobs;
}

}  // namespace Quaton
