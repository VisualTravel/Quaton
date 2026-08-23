#ifndef QUATON_BASE_DOWNLOAD_SCHEDULER_H_
#define QUATON_BASE_DOWNLOAD_SCHEDULER_H_

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

#include "quaton/base/download_job.h"
#include "quaton/base/download_state.h"
#include "quaton/quaton_global.h"

QUATON_NAMESPACE_BEGIN

/**
 * @struct SchedulerConfig
 * @brief Configuration for download scheduler
 */
struct SchedulerConfig {
  int max_concurrent_jobs = 4;    ///< Maximum concurrent jobs
  int max_concurrent_items = 16;  ///< Maximum concurrent items across all jobs
  int max_retries = 5;            ///< Maximum retries per item
  int retry_delay_ms = 1000;      ///< Delay between retries
  bool prioritize_small_files = true;  ///< Download small files first
};

/**
 * @brief Job priority comparator for priority queue
 */
struct JobPriorityComparator {
  bool operator()(const std::shared_ptr<DownloadJob>& a,
                  const std::shared_ptr<DownloadJob>& b) const {
    // Higher priority jobs should come first
    return static_cast<int>(a->GetItems().front()->GetPriority()) <
           static_cast<int>(b->GetItems().front()->GetPriority());
  }
};

/**
 * @class DownloadScheduler
 * @brief Manages and schedules download jobs
 *
 * Features:
 * - Priority-based job scheduling
 * - Concurrent job execution
 * - Automatic retry handling
 * - Progress aggregation
 */
class QUATON_API DownloadScheduler {
 public:
  /**
   * @brief Constructor
   * @param config Scheduler configuration
   */
  explicit DownloadScheduler(const SchedulerConfig& config = SchedulerConfig());

  /**
   * @brief Destructor - stops scheduler
   */
  ~DownloadScheduler();

  // Non-copyable
  DownloadScheduler(const DownloadScheduler&) = delete;
  DownloadScheduler& operator=(const DownloadScheduler&) = delete;

  // ========== Lifecycle ==========

  /**
   * @brief Start the scheduler
   */
  void Start();

  /**
   * @brief Stop the scheduler
   * @param wait_for_completion Wait for running jobs to complete
   */
  void Stop(bool wait_for_completion = true);

  /**
   * @brief Pause all jobs
   */
  void PauseAll();

  /**
   * @brief Resume all jobs
   */
  void ResumeAll();

  /**
   * @brief Cancel all jobs
   */
  void CancelAll();

  // ========== Job Management ==========

  /**
   * @brief Submit a job for execution
   * @param job Job to submit
   * @return true if job was accepted
   */
  bool SubmitJob(std::shared_ptr<DownloadJob> job);

  /**
   * @brief Cancel a specific job
   * @param job_id Job ID to cancel
   * @return true if job was found and cancelled
   */
  bool CancelJob(const std::string& job_id);

  /**
   * @brief Get job by ID
   */
  std::shared_ptr<DownloadJob> GetJob(const std::string& job_id) const;

  /**
   * @brief Get all jobs
   */
  std::vector<std::shared_ptr<DownloadJob>> GetAllJobs() const;

  /**
   * @brief Get pending job count
   */
  size_t GetPendingJobCount() const;

  /**
   * @brief Get running job count
   */
  size_t GetRunningJobCount() const;

  // ========== Progress ==========

  /**
   * @brief Get aggregate progress
   * @param total_items Output: total items
   * @param completed_items Output: completed items
   * @param failed_items Output: failed items
   */
  void GetAggregateProgress(size_t& total_items,
                            size_t& completed_items,
                            size_t& failed_items) const;

  /**
   * @brief Set global progress callback
   */
  void SetProgressCallback(
      std::function<void(size_t, size_t, size_t)> callback);

  // ========== Configuration ==========

  /**
   * @brief Update configuration
   */
  void UpdateConfig(const SchedulerConfig& config);

  /**
   * @brief Get current configuration
   */
  SchedulerConfig GetConfig() const;

  /**
   * @brief Check if scheduler is running
   */
  bool IsRunning() const { return running_.load(); }

 private:
  /**
   * @brief Main scheduler loop
   */
  void SchedulerLoop();

  /**
   * @brief Process next job from queue
   */
  void ProcessNextJob();

  /**
   * @brief Handle job completion
   */
  void OnJobCompleted(std::shared_ptr<DownloadJob> job, bool success);

  /**
   * @brief Check if can start more jobs
   */
  bool CanStartMoreJobs() const;

  SchedulerConfig config_;
  std::atomic<bool> running_{false};
  std::atomic<bool> paused_{false};

  // Job queues
  std::priority_queue<std::shared_ptr<DownloadJob>,
                      std::vector<std::shared_ptr<DownloadJob>>,
                      JobPriorityComparator>
      pending_jobs_;
  std::unordered_map<std::string, std::shared_ptr<DownloadJob>> running_jobs_;
  std::unordered_map<std::string, std::shared_ptr<DownloadJob>> all_jobs_;

  // Threading
  std::thread scheduler_thread_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;

  // Progress
  std::function<void(size_t, size_t, size_t)> progress_callback_;
};

QUATON_NAMESPACE_END

#endif  // QUATON_BASE_DOWNLOAD_SCHEDULER_H_
