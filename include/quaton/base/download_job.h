#ifndef QUATON_BASE_DOWNLOAD_JOB_H_
#define QUATON_BASE_DOWNLOAD_JOB_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "quaton/base/download_item.h"
#include "quaton/base/download_state.h"
#include "quaton/http_client.h"
#include "quaton/quaton_global.h"

QUATON_NAMESPACE_BEGIN

/**
 * @brief Job completion callback type
 */
using JobCompletionCallback =
    std::function<void(bool success, const std::string& error_msg)>;

/**
 * @brief Job progress callback type
 */
using JobProgressCallback =
    std::function<void(int64_t processed, int64_t total, double speed)>;

/**
 * @class DownloadJob
 * @brief Base class for download jobs
 *
 * A Job is responsible for:
 * - Processing one or more DownloadItems
 * - Managing HTTP connections
 * - Handling retries
 * - Reporting progress
 */
class QUATON_API DownloadJob {
 public:
  /**
   * @brief Constructor
   * @param job_id Unique job identifier
   * @param http_client Shared HTTP client
   */
  DownloadJob(const std::string& job_id,
              std::shared_ptr<HttpClient> http_client);

  /**
   * @brief Virtual destructor
   */
  virtual ~DownloadJob() = default;

  // Non-copyable
  DownloadJob(const DownloadJob&) = delete;
  DownloadJob& operator=(const DownloadJob&) = delete;

  // ========== Lifecycle ==========

  /**
   * @brief Start the job
   * @return true if started successfully
   */
  virtual bool Start();

  /**
   * @brief Pause the job
   */
  virtual void Pause();

  /**
   * @brief Resume the job
   */
  virtual void Resume();

  /**
   * @brief Cancel the job
   */
  virtual void Cancel();

  /**
   * @brief Wait for job completion
   * @param timeout_ms Timeout in milliseconds (0 = infinite)
   * @return true if job completed within timeout
   */
  virtual bool WaitForCompletion(int timeout_ms = 0);

  // ========== State ==========

  /**
   * @brief Get job state
   */
  JobState GetState() const { return state_.load(); }

  /**
   * @brief Check if job is running
   */
  bool IsRunning() const { return state_.load() == JobState::kRunning; }

  /**
   * @brief Check if job is complete
   */
  bool IsComplete() const {
    auto s = state_.load();
    return s == JobState::kCompleted || s == JobState::kFailed ||
           s == JobState::kCancelled;
  }

  // ========== Items ==========

  /**
   * @brief Add item to job
   * @param item Download item to add
   */
  virtual void AddItem(std::shared_ptr<DownloadItem> item);

  /**
   * @brief Get all items
   */
  const std::vector<std::shared_ptr<DownloadItem>>& GetItems() const {
    return items_;
  }

  /**
   * @brief Get item count
   */
  size_t GetItemCount() const { return items_.size(); }

  /**
   * @brief Get completed item count
   */
  size_t GetCompletedItemCount() const { return completed_count_.load(); }

  /**
   * @brief Get failed item count
   */
  size_t GetFailedItemCount() const { return failed_count_.load(); }

  // ========== Callbacks ==========

  /**
   * @brief Set completion callback
   */
  void SetCompletionCallback(JobCompletionCallback callback) {
    completion_callback_ = std::move(callback);
  }

  /**
   * @brief Set progress callback
   */
  void SetProgressCallback(JobProgressCallback callback) {
    progress_callback_ = std::move(callback);
  }

  // ========== Properties ==========

  /**
   * @brief Get job ID
   */
  const std::string& GetJobId() const { return job_id_; }

  /**
   * @brief Get/Set max concurrent items
   */
  int GetMaxConcurrent() const { return max_concurrent_; }
  void SetMaxConcurrent(int count) { max_concurrent_ = count; }

  /**
   * @brief Get/Set max retries per item
   */
  int GetMaxRetries() const { return max_retries_; }
  void SetMaxRetries(int count) { max_retries_ = count; }

 protected:
  /**
   * @brief Process single item (to be implemented by subclass)
   * @param item Item to process
   * @return true if processing succeeded
   */
  virtual bool ProcessItem(std::shared_ptr<DownloadItem> item) = 0;

  /**
   * @brief Called when all items are processed
   */
  virtual void OnAllItemsProcessed();

  /**
   * @brief Called when an item completes
   */
  virtual void OnItemCompleted(std::shared_ptr<DownloadItem> item);

  /**
   * @brief Called when an item fails
   */
  virtual void OnItemFailed(std::shared_ptr<DownloadItem> item);

  /**
   * @brief Transition job state
   */
  bool TransitionTo(JobState new_state);

  /**
   * @brief Report progress
   */
  void ReportProgress(int64_t processed, int64_t total, double speed);

  std::string job_id_;
  std::shared_ptr<HttpClient> http_client_;
  std::vector<std::shared_ptr<DownloadItem>> items_;
  std::atomic<JobState> state_{JobState::kCreated};
  std::atomic<size_t> completed_count_{0};
  std::atomic<size_t> failed_count_{0};
  std::atomic<bool> cancelled_{false};
  int max_concurrent_ = 4;
  int max_retries_ = 5;

  JobCompletionCallback completion_callback_;
  JobProgressCallback progress_callback_;

  mutable std::mutex mutex_;
  std::condition_variable completion_cv_;
};

QUATON_NAMESPACE_END

#endif  // QUATON_BASE_DOWNLOAD_JOB_H_
