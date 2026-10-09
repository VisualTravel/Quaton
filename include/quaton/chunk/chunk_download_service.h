#ifndef QUATON_CHUNK_CHUNK_DOWNLOAD_SERVICE_H_
#define QUATON_CHUNK_CHUNK_DOWNLOAD_SERVICE_H_

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "quaton/base/download_service.h"
#include "quaton/base/task_queue.h"
#include "quaton/chunk/chunk_download_item.h"
#include "quaton/chunk/chunk_download_manager.h"
#include "quaton/manifest/chunk_manifest_pair.h"
#include "quaton/progress_callback.h"
#include "quaton/quaton_global.h"
#include "quaton/resource.h"

QUATON_NAMESPACE_BEGIN

class HttpClient;

/**
 * @struct ChunkServiceConfig
 * @brief Configuration for chunk download service
 */
struct QUATON_API ChunkServiceConfig : public ServiceConfig {
  ChunkManagerConfig manager_config;
  std::string install_path;           ///< Installation directory
  std::string temp_path;              ///< Temporary download directory
  std::string manifest_output_dir;    ///< Directory to save manifests
  bool cleanup_temp_files = true;     ///< Remove temp files after success
  int download_timeout_minutes = 60;  ///< Download timeout in minutes
};

/**
 * @struct DownloadContext
 * @brief Download context structure for shared state during downloads
 */
struct DownloadContext {
  std::string output_directory;                    ///< Output directory
  std::string filter_criteria;                     ///< Filter criteria
  ChunkManifestPair manifest_pair_from;            ///< Source manifest
  ChunkManifestPair manifest_pair_to;              ///< Target manifest
  std::shared_ptr<std::atomic<bool>> cancel_flag;  ///< Cancel flag
  std::shared_ptr<std::mutex> console_mutex;       ///< Console output mutex
  std::shared_ptr<std::mutex> directory_mutex;     ///< Directory creation mutex
  std::shared_ptr<std::atomic<size_t>> skipped_downloads;  ///< Skipped count
  std::shared_ptr<std::atomic<size_t>>
      completed_downloads;                               ///< Completed count
  std::shared_ptr<std::atomic<size_t>> total_downloads;  ///< Total count
  /// Skip the local record fast path and transfer every task, as a repair does
  bool force_download = false;
  TaskQueue* download_queue;          ///< Download queue pointer
  ProgressTracker* progress_tracker;  ///< Progress tracker pointer
};

/**
 * @class ChunkDownloadService
 * @brief High-level service for chunk-based downloads
 *
 * Provides complete functionality for downloading game resources:
 * - Full game downloads
 * - Voice pack downloads
 * - Resource updates
 * - File restoration
 * - Database integration
 */
class QUATON_API ChunkDownloadService : public DownloadService {
 public:
  ChunkDownloadService();
  ~ChunkDownloadService() override;

  // Callbacks
  using FileCompletedCallback =
      std::function<void(const std::string& file_name, bool success)>;
  using AllCompletedCallback = std::function<void(bool all_success)>;
  using VerificationCallback =
      std::function<void(const std::string& file_name, bool verified)>;

  // ========== Local integrity report ==========

  /**
   * @struct LocalFileIssue
   * @brief One file that failed the local integrity check
   */
  struct LocalFileIssue {
    std::string file_name;   ///< Manifest relative path
    std::string checksum;    ///< MD5 the manifest lists for the file
    int64_t size = 0;        ///< Size in bytes the manifest lists
    int64_t actual_size = 0; ///< Size found on disk, 0 when unreadable
    bool missing = false;    ///< true when the file is absent on disk
  };

  /**
   * @struct LocalVerifyReport
   * @brief Outcome of a read-only local integrity scan
   */
  struct LocalVerifyReport {
    /// Input: cap for issues, 0 collects every failing file
    size_t max_issues = 5000;

    size_t total_files = 0;      ///< Files listed by the manifest
    size_t valid_files = 0;      ///< Files matching size and checksum
    size_t missing_files = 0;    ///< Files that are not on disk
    size_t corrupted_files = 0;  ///< Files present but not matching
    int64_t missing_bytes = 0;   ///< Manifest bytes of the missing files
    int64_t corrupted_bytes = 0; ///< Manifest bytes of the corrupted files
    bool issues_truncated = false;  ///< True when issues was capped
    bool cancelled = false;         ///< True when the scan was cancelled

    std::vector<LocalFileIssue> issues;  ///< Files that failed the check
  };

  // ========== Initialization ==========

  /**
   * @brief Initialize with config
   */
  bool Initialize(const ChunkServiceConfig& config);

  /**
   * @brief Get current config
   */
  const ChunkServiceConfig& GetChunkServiceConfig() const {
    return chunk_service_config_;
  }

  /**
   * @brief Stop the download currently running
   *
   * Sets the cancel flag of the active Download() call, so the worker threads
   * abandon the remaining resources and the call returns a failure code. Only
   * the manifest-URL overload is covered; the manifest-pair overload and
   * Restore() keep their cancel flag internal.
   */
  void CancelAll() override;

  // ========== Download Operations (URL Mode) ==========

  /**
   * @brief Download resources from manifest URLs (supports incremental updates)
   * @param previous_manifest_url Previous manifest file URL (optional, for
   * incremental)
   * @param current_manifest_url Current manifest file URL
   * @param chunk_base_url Base URL for chunk containers
   * @param output_directory Output directory
   * @param filter_criteria Filter criteria
   * @return Future with result code (0 = success)
   */
  std::future<int> Download(const std::string& previous_manifest_url,
                            const std::string& current_manifest_url,
                            const std::string& chunk_base_url,
                            const std::string& output_directory,
                            const std::string& filter_criteria);

  /**
   * @brief Download game resources using manifest information
   * @param manifest_pair Manifest information pair
   * @param package Package type ("game", "zh-cn", "en-us", etc.)
   * @param output_directory Output directory
   * @return Future with result code (0 = success)
   */
  std::future<int> Download(const ChunkManifestPair& manifest_pair,
                            const std::string& package,
                            const std::string& output_directory);

  /**
   * @brief Check the installed tree against a manifest without downloading
   *
   * Compares every file the manifest lists with the local tree: a file is valid
   * when it exists with the size and MD5 the manifest carries, which is what a
   * download of that manifest verifies too. The manifest is the only reference,
   * so a tree installed by other means, or one whose database records were
   * lost, can be checked just as well.
   *
   * Nothing is written to the install directory and no record is created.
   *
   * @param manifest_pair Target manifest information pair
   * @param output_directory Output directory (game install path)
   * @param report Receives the outcome; report->max_issues caps report->issues
   * @param cancel Optional flag the scan watches between files
   * @return true when the tree could be scanned, false when the directory is
   *         missing, the manifest could not be read or the scan was cancelled
   */
  bool Verify(const ChunkManifestPair& manifest_pair,
              const std::string& output_directory,
              LocalVerifyReport* report,
              const std::shared_ptr<std::atomic<bool>>& cancel = nullptr);

  /**
   * @brief Restore missing or corrupted files
   *
   * Downloads exactly the files Verify() reports and records them again.
   *
   * @param manifest_pair Manifest information pair
   * @param output_directory Output directory (game install path)
   * @param package Package name recorded for the repaired files, so it matches
   *                what the download that installed the tree wrote
   * @param cancel Optional flag the scan and the transfers watch
   * @return Future with result code (0 = success)
   */
  std::future<int> Restore(const ChunkManifestPair& manifest_pair,
                           const std::string& output_directory,
                           const std::string& package = "game",
                           const std::shared_ptr<std::atomic<bool>>& cancel =
                               nullptr);

  // ========== Download Operations (Resource Mode) ==========

  /**
   * @brief Start downloading resources
   * @param resources List of resources to download
   * @return Session ID for tracking
   */
  std::string StartDownload(const std::vector<Resource>& resources);

  /**
   * @brief Start downloading a single resource
   */
  std::string StartDownload(const Resource& resource);

  /**
   * @brief Pause downloads for a session
   */
  bool PauseDownload(const std::string& session_id);

  /**
   * @brief Resume paused downloads
   */
  bool ResumeDownload(const std::string& session_id);

  /**
   * @brief Cancel downloads for a session
   */
  bool CancelDownload(const std::string& session_id);

  // ========== Callbacks ==========

  /**
   * @brief Set progress callback
   */
  void SetProgressCallback(ProgressCallback callback);

  /**
   * @brief Set callback for individual file completion
   */
  void SetFileCompletedCallback(FileCompletedCallback callback) {
    file_completed_callback_ = std::move(callback);
  }

  /**
   * @brief Set callback for all files completed
   */
  void SetAllCompletedCallback(AllCompletedCallback callback) {
    all_completed_callback_ = std::move(callback);
  }

  /**
   * @brief Set callback for file verification
   */
  void SetVerificationCallback(VerificationCallback callback) {
    verification_callback_ = std::move(callback);
  }

  // ========== Status ==========

  /**
   * @brief Get download progress for a session
   */
  struct SessionProgress {
    std::string session_id;
    size_t total_files = 0;
    size_t completed_files = 0;
    size_t failed_files = 0;
    int64_t total_bytes = 0;
    int64_t downloaded_bytes = 0;
    double download_speed = 0.0;
    double progress_percentage = 0.0;
    std::chrono::seconds estimated_time_remaining{0};
  };

  SessionProgress GetSessionProgress(const std::string& session_id) const;

  /**
   * @brief Get all active sessions
   */
  std::vector<std::string> GetActiveSessions() const;

  // ========== Manager Access ==========

  /**
   * @brief Get underlying manager
   */
  std::shared_ptr<ChunkDownloadManager> GetManager() const {
    return chunk_manager_;
  }

 private:
  std::string GenerateSessionId();
  void OnFileCompleted(const std::string& file_name, bool success);
  void CheckAllCompleted(const std::string& session_id);

  /**
   * @brief Enumerate a manifest and split its files into valid and invalid
   *
   * Shared by Verify() and Restore() so both apply identical rules.
   *
   * @param invalid Receives the resources that need (re)downloading, may be
   *                null when the caller only wants the report
   * @param report Receives the outcome, may be null
   * @param cancel Optional flag the scan watches between files
   * @return true when the manifest could be enumerated
   */
  bool CollectInvalidResources(const ChunkManifestPair& manifest_pair,
                               const std::string& output_directory,
                               std::vector<std::shared_ptr<Resource>>* invalid,
                               LocalVerifyReport* report,
                               const std::shared_ptr<std::atomic<bool>>& cancel);

  /**
   * @brief Process a single download task
   */
  void ProcessDownloadTask(const std::shared_ptr<Resource>& asset,
                           DownloadContext& context,
                           const ChunkManifestPair& manifest_pair,
                           const std::string& package);

  /**
   * @brief Wait for download completion with timeout
   */
  void WaitForDownloadCompletion(TaskQueue& download_queue,
                                 DownloadContext& context);

  /**
   * @brief Save file record to database
   */
  void SaveFileRecordToDatabase(const std::shared_ptr<Resource>& asset,
                                const DownloadContext& context,
                                const ChunkManifestPair& manifest_pair,
                                const std::string& package,
                                bool is_successful,
                                const std::string& xxhash_checksum);

  /**
   * @brief Save manifest and config records to database
   */
  void SaveManifestAndConfigRecords(const ChunkManifestPair& manifest_pair,
                                    const std::string& package,
                                    const std::string& output_directory);

  ChunkServiceConfig chunk_service_config_;
  std::shared_ptr<ChunkDownloadManager> chunk_manager_;
  std::shared_ptr<HttpClient> http_client_;

  FileCompletedCallback file_completed_callback_;
  AllCompletedCallback all_completed_callback_;
  VerificationCallback verification_callback_;
  ProgressCallback progress_callback_;
  std::unique_ptr<ProgressTracker> progress_tracker_;

  // Session tracking
  struct SessionInfo {
    std::vector<std::string> file_names;
    size_t completed_count = 0;
    size_t failed_count = 0;
    std::chrono::steady_clock::time_point start_time;
  };

  mutable std::mutex sessions_mutex_;
  std::unordered_map<std::string, SessionInfo> sessions_;
  std::atomic<uint64_t> session_counter_{0};

  // Cancel flag of the running Download() call, empty while idle.
  std::mutex cancel_flag_mutex_;
  std::shared_ptr<std::atomic<bool>> active_cancel_flag_;
};

/**
 * @brief Application exit trigger, monitors user cancellation
 */
void AppExitTrigger(std::shared_ptr<std::atomic<bool>> cancel_flag);

QUATON_NAMESPACE_END

#endif  // QUATON_CHUNK_CHUNK_DOWNLOAD_SERVICE_H_
