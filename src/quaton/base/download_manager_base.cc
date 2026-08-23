#include "quaton/base/download_manager_base.h"

#include <sstream>

#include "quaton/logger.h"

namespace Quaton {

DownloadManagerBase::DownloadManagerBase() = default;

DownloadManagerBase::~DownloadManagerBase() {
  Shutdown();
}

bool DownloadManagerBase::Initialize(const ManagerConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (initialized_) {
    LOG_WARN("Manager: Already initialized");
    return true;
  }

  config_ = config;
  initialized_ = true;
  LOG_INFO("Manager: Initialized successfully");
  return true;
}

void DownloadManagerBase::Shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_) {
    return;
  }

  for (auto& [id, job] : jobs_) {
    if (job) {
      job->Cancel();
    }
  }
  jobs_.clear();
  initialized_ = false;
  LOG_INFO("Manager: Shutdown completed");
}

std::string DownloadManagerBase::SubmitJob(std::shared_ptr<DownloadJob> job,
                                           TaskPriority priority) {
  if (!initialized_) {
    LOG_ERROR("Manager: Not initialized");
    return "";
  }

  if (!job) {
    LOG_ERROR("Manager: Invalid job");
    return "";
  }

  std::string job_id = job->GetJobId();
  RegisterJob(job_id, job);

  job->Start();

  LOG_INFO("Manager: Submitted job %s", job_id.c_str());
  return job_id;
}

bool DownloadManagerBase::CancelJob(const std::string& job_id) {
  auto job = GetJob(job_id);
  if (!job) {
    LOG_WARN("Manager: Job %s not found", job_id.c_str());
    return false;
  }

  job->Cancel();
  UnregisterJob(job_id);

  LOG_INFO("Manager: Cancelled job %s", job_id.c_str());
  return true;
}

std::shared_ptr<DownloadJob> DownloadManagerBase::GetJob(
    const std::string& job_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = jobs_.find(job_id);
  return it != jobs_.end() ? it->second : nullptr;
}

std::vector<std::shared_ptr<DownloadJob>> DownloadManagerBase::GetAllJobs()
    const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<DownloadJob>> result;
  result.reserve(jobs_.size());
  for (const auto& [id, job] : jobs_) {
    result.push_back(job);
  }
  return result;
}

size_t DownloadManagerBase::GetTotalItemCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t total = 0;
  for (const auto& [id, job] : jobs_) {
    total += job->GetItemCount();
  }
  return total;
}

size_t DownloadManagerBase::GetCompletedItemCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t completed = 0;
  for (const auto& [id, job] : jobs_) {
    completed += job->GetCompletedItemCount();
  }
  return completed;
}

size_t DownloadManagerBase::GetFailedItemCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t failed = 0;
  for (const auto& [id, job] : jobs_) {
    failed += job->GetFailedItemCount();
  }
  return failed;
}

void DownloadManagerBase::RegisterJob(const std::string& job_id,
                                      std::shared_ptr<DownloadJob> job) {
  std::lock_guard<std::mutex> lock(mutex_);
  jobs_[job_id] = job;
  LOG_DEBUG("Manager: Registered job %s", job_id.c_str());
}

void DownloadManagerBase::UnregisterJob(const std::string& job_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  jobs_.erase(job_id);
  LOG_DEBUG("Manager: Unregistered job %s", job_id.c_str());
}

std::string DownloadManagerBase::GenerateJobId(const std::string& prefix) {
  uint64_t id = job_counter_++;

  std::ostringstream oss;
  oss << prefix << "_" << id;
  return oss.str();
}

void DownloadManagerBase::OnJobStateChanged(const std::string& job_id,
                                            JobState old_state,
                                            JobState new_state) {
  LOG_DEBUG("Manager: Job %s state changed: %d -> %d",
            job_id.c_str(),
            static_cast<int>(old_state),
            static_cast<int>(new_state));
}

void DownloadManagerBase::OnJobProgress(const std::string& job_id,
                                        int64_t downloaded,
                                        int64_t total) {
  // Default implementation: log progress
  if (total > 0) {
    double percentage = static_cast<double>(downloaded) / total * 100.0;
    LOG_DEBUG("Manager: Job %s progress: %.2f%%", job_id.c_str(), percentage);
  }
}

void DownloadManagerBase::OnJobCompleted(const std::string& job_id) {
  LOG_INFO("Manager: Job %s completed", job_id.c_str());
}

void DownloadManagerBase::OnJobFailed(const std::string& job_id,
                                      const std::string& error) {
  LOG_ERROR("Manager: Job %s failed: %s", job_id.c_str(), error.c_str());
}

}  // namespace Quaton
