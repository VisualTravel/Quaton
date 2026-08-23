#ifndef PROGRESS_CALLBACK_H
#define PROGRESS_CALLBACK_H
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>

namespace Quaton {

/**
 * @enum FileProcessStage
 * @brief File processing stage flags
 */
enum class FileProcessStage {
  kNone = 0,               ///< Not started
  kDownloadInitiated = 1,  ///< Download instance created
  kDownloadCompleted = 2,  ///< Download completed
  kDecompressed = 3,       ///< Decompression completed
  kVerified = 4,           ///< Verification completed
  kApplied = 5             ///< Applied (for update mode)
};

/**
 * @enum OperationMode
 * @brief Operation mode for progress tracking
 */
enum class OperationMode {
  kChunkDownload,     ///< Chunk-based download
  kPatchUpdate,       ///< Patch-based update (download and apply)
  kPatchPredownload,  ///< Patch-based predownload (download only)
  kPatchLocalInstall  ///< Apply patches from local storage
};

/**
 * @struct FileProcessStatus
 * @brief Individual file processing status
 */
struct FileProcessStatus {
  bool download_initiated;  ///< Download instance created
  bool download_completed;  ///< Download completed
  bool decompressed;        ///< Decompression completed
  bool verified;            ///< Verification completed
  bool applied;             ///< Applied (for update operations)

  FileProcessStatus()
      : download_initiated(false),
        download_completed(false),
        decompressed(false),
        verified(false),
        applied(false) {}

  /**
   * @brief Reset all flags to false
   */
  void reset() {
    download_initiated = false;
    download_completed = false;
    decompressed = false;
    verified = false;
    applied = false;
  }

  /**
   * @brief Set stage based on operation mode
   * @param stage Current processing stage
   * @param mode Operation mode
   */
  void set_stage(FileProcessStage stage, OperationMode mode) {
    switch (stage) {
      case FileProcessStage::kDownloadInitiated:
        download_initiated = true;
        break;
      case FileProcessStage::kDownloadCompleted:
        download_completed = true;
        break;
      case FileProcessStage::kDecompressed:
        decompressed = true;
        break;
      case FileProcessStage::kVerified:
        verified = true;
        break;
      case FileProcessStage::kApplied:
        applied = true;
        break;
      default:
        break;
    }
  }
};

/**
 * @struct ProgressInfo
 * @brief Unified progress information structure
 */
struct ProgressInfo {
  // Overall progress
  double overall_percentage;  ///< Overall progress percentage (0.0 - 1.0)

  // Current file information
  std::string current_file;  ///< Current file being processed (with path)
  FileProcessStatus current_status;  ///< Current file processing status

  // File counts
  int total_files;      ///< Total number of files
  int completed_files;  ///< Number of completed files
  int remaining_files;  ///< Number of remaining files
  int failed_files;     ///< Number of failed files

  // Download statistics
  double download_speed;            ///< Download speed (MB/s)
  double estimated_seconds;         ///< Estimated remaining time (seconds)
  int estimated_minutes;            ///< Estimated remaining time (minutes)
  int estimated_remaining_seconds;  ///< Remaining seconds after minutes

  // Operation context
  OperationMode operation_mode;  ///< Current operation mode

  ProgressInfo()
      : overall_percentage(0.0),
        total_files(0),
        completed_files(0),
        remaining_files(0),
        failed_files(0),
        download_speed(0.0),
        estimated_seconds(0.0),
        estimated_minutes(0),
        estimated_remaining_seconds(0),
        operation_mode(OperationMode::kChunkDownload) {}
};

/**
 * @typedef ProgressCallback
 * @brief Progress callback function type
 */
using ProgressCallback = std::function<void(const ProgressInfo&)>;

/**
 * @class ProgressTracker
 * @brief Progress tracking and management class
 *
 * Manages progress tracking for download/update operations with automatic
 * throttling to prevent performance issues (updates every 0.5 seconds)
 */
class ProgressTracker {
 public:
  /**
   * @brief Constructor
   * @param total_files Total number of files to process
   * @param mode Operation mode
   */
  explicit ProgressTracker(int total_files,
                           OperationMode mode = OperationMode::kChunkDownload);

  /**
   * @brief Destructor
   */
  ~ProgressTracker() = default;

  /**
   * @brief Set progress callback function
   * @param callback Callback function
   */
  void set_callback(ProgressCallback callback);

  /**
   * @brief Update current file being processed
   * @param file_path File path
   * @param stage Processing stage
   */
  void update_current_file(const std::string& file_path,
                           FileProcessStage stage);

  /**
   * @brief Mark file as completed
   */
  void increment_completed();

  /**
   * @brief Update download statistics
   * @param bytes_downloaded Total bytes downloaded
   * @param bytes_total Total bytes to download
   */
  void update_download_stats(int64_t bytes_downloaded, int64_t bytes_total);

  /**
   * @brief Force trigger callback (ignores throttling)
   */
  void force_update();

  /**
   * @brief Get current progress information
   * @return Current progress info
   */
  ProgressInfo get_progress() const;

  /**
   * @brief Reset tracker state
   */
  void reset();

 private:
  /**
   * @brief Calculate overall percentage
   */
  void calculate_percentage();

  /**
   * @brief Calculate estimated time
   */
  void calculate_estimated_time();

  /**
   * @brief Check if should trigger callback (throttling)
   * @return true if should trigger
   */
  bool should_trigger_callback();

  /**
   * @brief Trigger callback if conditions are met
   */
  void trigger_callback_if_ready();

  // Progress data
  std::atomic<int> total_files_;
  std::atomic<int> completed_files_;
  std::atomic<int64_t> bytes_downloaded_;
  std::atomic<int64_t> bytes_total_;

  // Current file tracking
  mutable std::mutex current_file_mutex_;
  std::string current_file_;
  FileProcessStatus current_status_;

  // Operation context
  OperationMode operation_mode_;

  // Callback management
  mutable std::mutex callback_mutex_;
  ProgressCallback callback_;

  // Throttling
  std::chrono::steady_clock::time_point last_callback_time_;
  std::chrono::steady_clock::time_point start_time_;
  static constexpr int kCallbackIntervalMs = 500;  ///< 0.5 seconds

  // Download speed tracking
  int64_t last_bytes_downloaded_;
  std::chrono::steady_clock::time_point last_speed_update_;
  double current_speed_;  ///< Bytes per second
};

}  // namespace Quaton

#endif  // PROGRESS_CALLBACK_H
