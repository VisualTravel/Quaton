#ifndef QUATON_BASE_DOWNLOAD_SERVICE_H_
#define QUATON_BASE_DOWNLOAD_SERVICE_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "quaton/base/download_scheduler.h"
#include "quaton/http_client.h"
#include "quaton/progress_callback.h"
#include "quaton/quaton_global.h"

QUATON_NAMESPACE_BEGIN

/**
 * @struct ServiceConfig
 * @brief Configuration for download service
 */
struct ServiceConfig {
  int thread_count = 0;            ///< Thread count (0 = auto)
  int max_http_handles = 128;      ///< Maximum HTTP handles
  int buffer_size = 32;            ///< Buffer size in MB
  int retry_count = 5;             ///< Retry count on failure
  int request_timeout_ms = 30000;  ///< Request timeout in ms
  bool verify_downloads = true;    ///< Verify downloads after completion
  bool silent = false;             ///< Silent mode (minimal logging)
};

/**
 * @struct ServiceStatistics
 * @brief Service-level statistics
 */
struct ServiceStatistics {
  size_t total_jobs = 0;
  size_t completed_jobs = 0;
  size_t failed_jobs = 0;
  size_t total_items = 0;
  size_t completed_items = 0;
  size_t failed_items = 0;
  size_t skipped_items = 0;
  int64_t total_bytes = 0;
  int64_t downloaded_bytes = 0;
  double current_speed = 0.0;
  int64_t elapsed_ms = 0;
};

/**
 * @class DownloadService
 * @brief Base class for download services
 *
 * Provides:
 * - High-level download API
 * - Scheduler management
 * - Progress aggregation
 * - Error handling
 */
class QUATON_API DownloadService {
 public:
  /**
   * @brief Constructor
   * @param config Service configuration
   */
  explicit DownloadService(const ServiceConfig& config = ServiceConfig());

  /**
   * @brief Virtual destructor
   */
  virtual ~DownloadService();

  // Non-copyable
  DownloadService(const DownloadService&) = delete;
  DownloadService& operator=(const DownloadService&) = delete;

  // ========== Lifecycle ==========

  /**
   * @brief Initialize the service
   * @return true if initialization succeeded
   */
  virtual bool Initialize();

  /**
   * @brief Shutdown the service
   */
  virtual void Shutdown();

  /**
   * @brief Check if service is initialized
   */
  bool IsInitialized() const { return initialized_.load(); }

  /**
   * @brief Check if service is busy
   */
  bool IsBusy() const;

  // ========== Operations ==========

  /**
   * @brief Cancel all operations
   */
  virtual void CancelAll();

  /**
   * @brief Pause all operations
   */
  virtual void PauseAll();

  /**
   * @brief Resume all operations
   */
  virtual void ResumeAll();

  // ========== Progress ==========

  /**
   * @brief Set progress callback
   */
  void SetProgressCallback(ProgressCallback callback);

  /**
   * @brief Get current statistics
   */
  ServiceStatistics GetStatistics() const;

  // ========== Configuration ==========

  /**
   * @brief Get service configuration
   */
  const ServiceConfig& GetConfig() const { return config_; }

  /**
   * @brief Update configuration
   */
  void UpdateConfig(const ServiceConfig& config);

 protected:
  /**
   * @brief Create HTTP client
   */
  std::shared_ptr<HttpClient> CreateHttpClient();

  /**
   * @brief Get scheduler
   */
  DownloadScheduler& GetScheduler() { return *scheduler_; }

  /**
   * @brief Update statistics
   */
  void UpdateStatistics(const ServiceStatistics& delta);

  /**
   * @brief Report progress
   */
  void ReportProgress();

  ServiceConfig config_;
  std::unique_ptr<DownloadScheduler> scheduler_;
  std::shared_ptr<HttpClient> http_client_;
  ProgressCallback progress_callback_;

  mutable std::mutex mutex_;
  std::atomic<bool> initialized_{false};
  ServiceStatistics statistics_;
  std::chrono::steady_clock::time_point start_time_;
};

QUATON_NAMESPACE_END

#endif  // QUATON_BASE_DOWNLOAD_SERVICE_H_
