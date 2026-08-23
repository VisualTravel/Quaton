#ifndef QUATON_BASE_DOWNLOAD_MANAGER_H_
#define QUATON_BASE_DOWNLOAD_MANAGER_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "quaton/base/download_job.h"
#include "quaton/base/download_state.h"
#include "quaton/quaton_global.h"

QUATON_NAMESPACE_BEGIN

// Forward declarations
class DownloadScheduler;

/**
 * @struct ManagerConfig
 * @brief Configuration for download manager
 */
struct QUATON_API ManagerConfig {
  std::string download_path;             // Download destination path
  std::string temp_path;                 // Temporary files path
  int max_retry_count = 5;               // Max retry attempts
  bool enable_resume = true;             // Enable resume support
  bool enable_progress_report = true;    // Enable progress reporting
  bool verify_after_download = true;     // Verify after download
  size_t buffer_size = 4 * 1024 * 1024;  // 4MB buffer
};

/**
 * @class DownloadManagerBase
 * @brief Base class for download managers
 *
 * Responsibilities:
 * - Job creation and management
 * - Item parsing and distribution
 * - Coordination with scheduler
 */
class QUATON_API DownloadManagerBase {
 public:
  /**
   * @brief Default constructor
   */
  DownloadManagerBase();

  /**
   * @brief Virtual destructor
   */
  virtual ~DownloadManagerBase();

  // Non-copyable
  DownloadManagerBase(const DownloadManagerBase&) = delete;
  DownloadManagerBase& operator=(const DownloadManagerBase&) = delete;

  // ========== Initialization ==========

  /**
   * @brief Initialize with config
   */
  virtual bool Initialize(const ManagerConfig& config);

  /**
   * @brief Shutdown manager
   */
  virtual void Shutdown();

  /**
   * @brief Check if initialized
   */
  bool IsInitialized() const { return initialized_; }

  // ========== Job Management ==========

  /**
   * @brief Submit job for execution
   * @param job Job to submit
   * @param priority Job priority
   * @return Job ID
   */
  std::string SubmitJob(std::shared_ptr<DownloadJob> job,
                        TaskPriority priority = TaskPriority::kNormal);

  /**
   * @brief Cancel job
   * @param job_id Job ID to cancel
   * @return true if cancellation succeeded
   */
  bool CancelJob(const std::string& job_id);

  /**
   * @brief Get job by ID
   */
  std::shared_ptr<DownloadJob> GetJob(const std::string& job_id) const;

  /**
   * @brief Get all managed jobs
   */
  std::vector<std::shared_ptr<DownloadJob>> GetAllJobs() const;

  // ========== Progress ==========

  /**
   * @brief Get total item count
   */
  size_t GetTotalItemCount() const;

  /**
   * @brief Get completed item count
   */
  size_t GetCompletedItemCount() const;

  /**
   * @brief Get failed item count
   */
  size_t GetFailedItemCount() const;

  // ========== Configuration ==========

  /**
   * @brief Get configuration
   */
  const ManagerConfig& GetConfig() const { return config_; }

 protected:
  /**
   * @brief Register job for tracking
   */
  void RegisterJob(const std::string& job_id, std::shared_ptr<DownloadJob> job);

  /**
   * @brief Unregister job
   */
  void UnregisterJob(const std::string& job_id);

  /**
   * @brief Generate unique job ID
   */
  std::string GenerateJobId(const std::string& prefix = "job");

  /**
   * @brief Called when job state changes
   */
  virtual void OnJobStateChanged(const std::string& job_id,
                                 JobState old_state,
                                 JobState new_state);

  /**
   * @brief Called when job reports progress
   */
  virtual void OnJobProgress(const std::string& job_id,
                             int64_t downloaded,
                             int64_t total);

  /**
   * @brief Called when job completes
   */
  virtual void OnJobCompleted(const std::string& job_id);

  /**
   * @brief Called when job fails
   */
  virtual void OnJobFailed(const std::string& job_id, const std::string& error);

  ManagerConfig config_;
  std::unordered_map<std::string, std::shared_ptr<DownloadJob>> jobs_;
  mutable std::mutex mutex_;
  std::atomic<uint64_t> job_counter_{0};
  bool initialized_ = false;
};

QUATON_NAMESPACE_END

#endif  // QUATON_BASE_DOWNLOAD_MANAGER_H_
