#ifndef QUATON_CONFIGURATION_DOWNLOAD_CONFIG_H
#define QUATON_CONFIGURATION_DOWNLOAD_CONFIG_H
#pragma once

#include <atomic>

namespace Quaton {

/**
 * @struct DownloadSettings
 * @brief Download configuration settings structure
 */
struct DownloadSettings {
  int buffer_size = 32;               ///< Buffer size in KB
  int buffer_queue_size = 10240;      ///< Buffer queue size
  int retry_on_download_failure = 5;  ///< Retry count on failure
  int request_timeout_wait_time = 5;  ///< Request timeout wait time
  int max_concurrent_tasks = 0;       ///< Max concurrent tasks (0 = auto)
  int max_validation_threads = 0;     ///< Max validation threads (0 = auto)
  int max_http_handles = 128;         ///< Max HTTP handles
  int download_timeout_minutes =
      30;  ///< Download completion timeout in minutes
  bool multi_thread_io_read_enabled = true;   ///< Enable multi-thread I/O read
  bool multi_thread_io_write_enabled = true;  ///< Enable multi-thread I/O write

  /**
   * @brief Create default download settings
   * @return Default download settings
   */
  static DownloadSettings create_default() { return DownloadSettings{}; }

  /**
   * @brief Create high-performance download settings
   * @return High-performance download settings
   */
  static DownloadSettings create_high_performance() {
    DownloadSettings settings;
    settings.buffer_size = 64;
    settings.buffer_queue_size = 20480;
    settings.max_concurrent_tasks = 16;
    settings.max_validation_threads = 8;
    settings.max_http_handles = 256;
    return settings;
  }

  /**
   * @brief Create conservative download settings
   * @return Conservative download settings
   */
  static DownloadSettings create_conservative() {
    DownloadSettings settings;
    settings.buffer_size = 16;
    settings.buffer_queue_size = 5120;
    settings.retry_on_download_failure = 10;
    settings.max_concurrent_tasks = 4;
    settings.max_validation_threads = 2;
    settings.max_http_handles = 64;
    return settings;
  }
};

/**
 * @class DownloadConfig
 * @brief Global download configuration manager (singleton pattern)
 *
 * Thread-safe singleton for managing download configuration
 */
class DownloadConfig {
 public:
  /**
   * @brief Get singleton instance
   * @return Reference to DownloadConfig instance
   */
  static DownloadConfig& instance() {
    static DownloadConfig instance;
    return instance;
  }

  // Delete copy constructor and assignment operations
  DownloadConfig(const DownloadConfig&) = delete;
  DownloadConfig& operator=(const DownloadConfig&) = delete;

  // Getter methods
  int GetBufferSize() const { return buffer_size_.load(); }
  int GetBufferQueueSize() const { return buffer_queue_size_.load(); }
  int GetRetryOnDownloadFailure() const {
    return retry_on_download_failure_.load();
  }
  int GetRequestTimeoutWaitTime() const {
    return request_timeout_wait_time_.load();
  }
  int GetMaxConcurrentTasks() const { return max_concurrent_tasks_.load(); }
  int GetMaxValidationThreads() const { return max_validation_threads_.load(); }
  int GetMaxHttpHandles() const { return max_http_handles_.load(); }
  int GetDownloadTimeoutMinutes() const {
    return download_timeout_minutes_.load();
  }
  bool GetMultiThreadIoReadEnabled() const {
    return multi_thread_io_read_enabled_.load();
  }
  bool GetMultiThreadIoWriteEnabled() const {
    return multi_thread_io_write_enabled_.load();
  }

  // Setter methods
  void SetBufferSize(int value) { buffer_size_.store(value); }
  void SetBufferQueueSize(int value) { buffer_queue_size_.store(value); }
  void SetRetryOnDownloadFailure(int value) {
    retry_on_download_failure_.store(value);
  }
  void SetRequestTimeoutWaitTime(int value) {
    request_timeout_wait_time_.store(value);
  }
  void SetMaxConcurrentTasks(int value) { max_concurrent_tasks_.store(value); }
  void SetMaxValidationThreads(int value) {
    max_validation_threads_.store(value);
  }
  void SetMaxHttpHandles(int value) { max_http_handles_.store(value); }
  void SetDownloadTimeoutMinutes(int value) {
    download_timeout_minutes_.store(value);
  }
  void SetMultiThreadIoReadEnabled(bool value) {
    multi_thread_io_read_enabled_.store(value);
  }
  void SetMultiThreadIoWriteEnabled(bool value) {
    multi_thread_io_write_enabled_.store(value);
  }

  /**
   * @brief Apply settings to the configuration
   * @param settings Settings to apply
   */
  void ApplySettings(const DownloadSettings& settings) {
    buffer_size_.store(settings.buffer_size);
    buffer_queue_size_.store(settings.buffer_queue_size);
    retry_on_download_failure_.store(settings.retry_on_download_failure);
    request_timeout_wait_time_.store(settings.request_timeout_wait_time);
    max_concurrent_tasks_.store(settings.max_concurrent_tasks);
    max_validation_threads_.store(settings.max_validation_threads);
    max_http_handles_.store(settings.max_http_handles);
    download_timeout_minutes_.store(settings.download_timeout_minutes);
    multi_thread_io_read_enabled_.store(settings.multi_thread_io_read_enabled);
    multi_thread_io_write_enabled_.store(
        settings.multi_thread_io_write_enabled);
  }

  /**
   * @brief Reset all configurations to default values
   */
  void ResetToDefaults() { ApplySettings(DownloadSettings::create_default()); }

 private:
  DownloadConfig() { ResetToDefaults(); }

  // Configuration parameters (using atomic to ensure thread safety)
  std::atomic<int> buffer_size_;
  std::atomic<int> buffer_queue_size_;
  std::atomic<int> retry_on_download_failure_;
  std::atomic<int> request_timeout_wait_time_;
  std::atomic<int> max_concurrent_tasks_;
  std::atomic<int> max_validation_threads_;
  std::atomic<int> max_http_handles_;
  std::atomic<int> download_timeout_minutes_;
  std::atomic<bool> multi_thread_io_read_enabled_;
  std::atomic<bool> multi_thread_io_write_enabled_;
};

}  // namespace Quaton

#endif  // QUATON_CONFIGURATION_DOWNLOAD_CONFIG_H
