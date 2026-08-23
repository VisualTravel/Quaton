#ifndef QUATON_BASE_DOWNLOAD_ITEM_H_
#define QUATON_BASE_DOWNLOAD_ITEM_H_

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "quaton/base/download_state.h"
#include "quaton/quaton_global.h"

QUATON_NAMESPACE_BEGIN

/**
 * @struct DownloadProgress
 * @brief Download progress information
 */
struct DownloadProgress {
  int64_t downloaded_bytes = 0;    ///< Bytes downloaded so far
  int64_t total_bytes = 0;         ///< Total bytes to download
  double speed_bytes_per_sec = 0;  ///< Current download speed
  double percentage = 0.0;         ///< Progress percentage (0-100)
  int64_t eta_seconds = 0;         ///< Estimated time remaining

  void UpdatePercentage() {
    if (total_bytes > 0) {
      percentage =
          (static_cast<double>(downloaded_bytes) / total_bytes) * 100.0;
    }
  }
};

/**
 * @struct DownloadError
 * @brief Download error information
 */
struct DownloadError {
  int error_code = 0;
  std::string error_message;
  std::string error_detail;
  int retry_count = 0;
  bool is_retryable = true;

  bool HasError() const { return error_code != 0; }
  void Clear() {
    error_code = 0;
    error_message.clear();
    error_detail.clear();
  }
};

/**
 * @class DownloadItem
 * @brief Base class for downloadable items with state machine
 *
 * Provides:
 * - State machine management
 * - Progress tracking
 * - Error handling
 * - Retry logic
 */
class QUATON_API DownloadItem {
 public:
  /**
   * @brief Constructor
   * @param id Unique identifier for this item
   */
  explicit DownloadItem(const std::string& id);

  /**
   * @brief Virtual destructor
   */
  virtual ~DownloadItem() = default;

  // Non-copyable
  DownloadItem(const DownloadItem&) = delete;
  DownloadItem& operator=(const DownloadItem&) = delete;

  // Movable
  DownloadItem(DownloadItem&&) noexcept = default;
  DownloadItem& operator=(DownloadItem&&) noexcept = default;

  // ========== State Machine ==========

  /**
   * @brief Get current state
   */
  DownloadState GetState() const { return state_.load(); }

  /**
   * @brief Transition to new state
   * @param new_state Target state
   * @param reason Reason for transition
   * @return true if transition succeeded
   */
  bool TransitionTo(DownloadState new_state, const std::string& reason = "");

  /**
   * @brief Set state change callback
   */
  void SetStateChangeCallback(StateChangeCallback callback);

  /**
   * @brief Check if item is in a terminal state
   */
  bool IsTerminal() const;

  /**
   * @brief Check if item can be retried
   */
  bool CanRetry() const;

  // ========== Progress ==========

  /**
   * @brief Get current progress
   */
  DownloadProgress GetProgress() const;

  /**
   * @brief Update progress
   */
  void UpdateProgress(int64_t downloaded_bytes, int64_t total_bytes);

  /**
   * @brief Update download speed
   */
  void UpdateSpeed(double bytes_per_sec);

  // ========== Error Handling ==========

  /**
   * @brief Get last error
   */
  DownloadError GetError() const;

  /**
   * @brief Set error
   */
  void SetError(int code,
                const std::string& message,
                const std::string& detail = "",
                bool retryable = true);

  /**
   * @brief Clear error
   */
  void ClearError();

  /**
   * @brief Increment retry count
   * @return Current retry count after increment
   */
  int IncrementRetryCount();

  /**
   * @brief Get retry count
   */
  int GetRetryCount() const { return error_.retry_count; }

  // ========== Properties ==========

  /**
   * @brief Get item ID
   */
  const std::string& GetId() const { return id_; }

  /**
   * @brief Get/Set URL
   */
  const std::string& GetUrl() const { return url_; }
  void SetUrl(const std::string& url) { url_ = url; }

  /**
   * @brief Get/Set file path
   */
  const std::string& GetFilePath() const { return file_path_; }
  void SetFilePath(const std::string& path) { file_path_ = path; }

  /**
   * @brief Get/Set expected file size
   */
  int64_t GetExpectedSize() const { return expected_size_; }
  void SetExpectedSize(int64_t size) { expected_size_ = size; }

  /**
   * @brief Get/Set expected checksum
   */
  const std::string& GetExpectedChecksum() const { return expected_checksum_; }
  void SetExpectedChecksum(const std::string& checksum) {
    expected_checksum_ = checksum;
  }

  /**
   * @brief Get/Set priority
   */
  TaskPriority GetPriority() const { return priority_; }
  void SetPriority(TaskPriority priority) { priority_ = priority; }

  /**
   * @brief Get state transition history
   */
  std::vector<StateTransition> GetStateHistory() const;

 protected:
  /**
   * @brief Called when state changes (for subclass hooks)
   */
  virtual void OnStateChanged(DownloadState old_state,
                              DownloadState new_state) {}

  std::string id_;                 ///< Unique identifier
  std::string url_;                ///< Download URL
  std::string file_path_;          ///< Target file path
  int64_t expected_size_ = 0;      ///< Expected file size
  std::string expected_checksum_;  ///< Expected checksum (MD5/XXHash)
  TaskPriority priority_ = TaskPriority::kNormal;

 private:
  std::atomic<DownloadState> state_{DownloadState::kIdle};
  mutable std::mutex mutex_;
  DownloadProgress progress_;
  DownloadError error_;
  StateChangeCallback state_callback_;
  std::vector<StateTransition> state_history_;
  int max_retry_count_ = 5;
};

QUATON_NAMESPACE_END

#endif  // QUATON_BASE_DOWNLOAD_ITEM_H_
