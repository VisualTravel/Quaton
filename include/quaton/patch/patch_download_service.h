#ifndef QUATON_PATCH_PATCH_DOWNLOAD_SERVICE_H_
#define QUATON_PATCH_PATCH_DOWNLOAD_SERVICE_H_

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "quaton/base/download_service.h"
#include "quaton/patch/patch_download_item.h"
#include "quaton/patch/patch_download_manager.h"
#include "quaton/quaton_global.h"

QUATON_NAMESPACE_BEGIN

class HttpClient;
class PatchManifestProcessor;
struct PatchInfo;
struct PatchFileMetadata;

// Minimum buffer size for patch download pipeline
constexpr int kPatchMinBufferSize = 3;

/**
 * @enum PatchDownloadMode
 * @brief Mode for patch download operations
 */
enum class PatchDownloadMode {
  kPreDownload,      ///< Download all patches first, apply later
  kDownloadAndApply  ///< Download and apply in pipeline
};

/**
 * @struct PatchServiceConfig
 * @brief Configuration for patch download service
 */
struct QUATON_API PatchServiceConfig : public ServiceConfig {
  PatchManagerConfig manager_config;
  std::string backup_path;          // Backup original files before patch
  std::string temp_patch_dir;       // Temporary directory for patches
  bool create_backup = true;        // Whether to backup
  bool rollback_on_failure = true;  // Rollback if patch fails
  PatchDownloadMode mode = PatchDownloadMode::kDownloadAndApply;
  int download_threads = 0;  // 0 = auto
  int apply_threads = 0;     // 0 = auto
  int max_buffer_size = 10;  // Max patches in buffer
};

/**
 * @struct PipelinePatchItem
 * @brief Internal structure for pipeline patch processing
 */
struct PipelinePatchItem {
  std::string patch_identifier;
  std::string download_url;
  std::string storage_path;
  int64_t file_size;
  std::string checksum_md5;
  bool download_started = false;
  bool download_completed = false;
  bool apply_completed = false;
  bool verified = false;
  std::vector<const PatchFileMetadata*> associated_files;

  PipelinePatchItem() = default;
  PipelinePatchItem(const std::string& id,
                    const std::string& url,
                    const std::string& path,
                    int64_t size,
                    const std::string& md5)
      : patch_identifier(id),
        download_url(url),
        storage_path(path),
        file_size(size),
        checksum_md5(md5) {}
};

/**
 * @class PatchDownloadService
 * @brief High-level service for patch-based updates
 *
 * Provides simplified API for incremental updates:
 * - Download patches
 * - Apply patches to source files
 * - Verify results
 * - Handle rollback on failure
 *
 * This class completely replaces the old PatchDownloader functionality.
 */
class QUATON_API PatchDownloadService : public DownloadService {
 public:
  PatchDownloadService();
  ~PatchDownloadService() override;

  // Callbacks
  using ProgressCallback = std::function<void(const ProgressInfo&)>;
  using PatchAppliedCallback =
      std::function<void(const std::string& file_name, bool success)>;
  using AllPatchesAppliedCallback = std::function<void(bool all_success)>;
  using ApplyProgressCallback =
      std::function<void(const std::string& file_name, double progress)>;

  // ========== Initialization ==========

  bool Initialize(const PatchServiceConfig& config);

  const PatchServiceConfig& GetPatchServiceConfig() const {
    return patch_service_config_;
  }

  // ========== Main API (async) ==========

  /**
   * @brief Prepare download tasks from patch manifest
   * @param manifest Parsed patch manifest
   * @param package_name Package name
   * @param download_url_prefix URL prefix for patch downloads
   * @param download_url_suffix URL suffix for patch downloads
   * @return Number of patches to download
   */
  int PrepareDownloadTasks(
      const std::shared_ptr<PatchManifestProcessor>& manifest,
      const std::string& package_name,
      const std::string& download_url_prefix,
      const std::string& download_url_suffix);

  /**
   * @brief Execute patch download and apply
   * @return future with result code
   */
  std::future<int> Execute();

  /**
   * @brief Apply downloaded patches (for PreDownload mode)
   * @return future with result code
   */
  std::future<int> ApplyDownloadedPatches();

  /**
   * @brief Cancel current operation
   */
  void Cancel();

  // ========== Legacy Session-based API ==========

  /**
   * @brief Start applying patches (session-based)
   * @param patches List of patch info
   * @return Session ID
   */
  std::string StartPatchUpdate(const std::vector<PatchInfo>& patches);

  /**
   * @brief Start applying a single patch (session-based)
   */
  std::string StartPatchUpdate(const PatchInfo& patch);

  /**
   * @brief Pause patch operations
   */
  bool PausePatchUpdate(const std::string& session_id);

  /**
   * @brief Resume patch operations
   */
  bool ResumePatchUpdate(const std::string& session_id);

  /**
   * @brief Cancel and rollback
   */
  bool CancelPatchUpdate(const std::string& session_id);

  // ========== Callbacks ==========

  void SetProgressCallback(ProgressCallback callback);

  void SetPatchAppliedCallback(PatchAppliedCallback callback) {
    patch_applied_callback_ = std::move(callback);
  }

  void SetAllPatchesAppliedCallback(AllPatchesAppliedCallback callback) {
    all_patches_applied_callback_ = std::move(callback);
  }

  void SetApplyProgressCallback(ApplyProgressCallback callback) {
    apply_progress_callback_ = std::move(callback);
  }

  // ========== Setters ==========

  /**
   * @brief Set HTTP client for downloads
   */
  void SetHttpClient(std::shared_ptr<HttpClient> client) {
    http_client_ = std::move(client);
  }

  /**
   * @brief Set installation path (game directory)
   */
  void SetInstallPath(const std::string& path) { output_dir_ = path; }

  // ========== Status ==========

  struct PatchSessionProgress {
    std::string session_id;
    size_t total_patches = 0;
    size_t downloaded_patches = 0;
    size_t applied_patches = 0;
    size_t failed_patches = 0;
    int64_t total_bytes = 0;
    int64_t downloaded_bytes = 0;
    double download_progress = 0.0;
    double apply_progress = 0.0;
    double overall_progress = 0.0;
    std::chrono::seconds estimated_time_remaining{0};
  };

  PatchSessionProgress GetSessionProgress(const std::string& session_id) const;

  std::vector<std::string> GetActiveSessions() const;

  // ========== Statistics ==========

  int GetDownloadedCount() const { return downloaded_count_.load(); }
  int GetAppliedCount() const { return applied_count_.load(); }
  int64_t GetDownloadedBytes() const { return downloaded_bytes_.load(); }
  int64_t GetTotalBytes() const { return total_bytes_; }
  bool HasError() const { return has_error_.load(); }
  bool IsCancelled() const { return cancelled_.load(); }

  // ========== Manager Access ==========

  std::shared_ptr<PatchDownloadManager> GetManager() const {
    return patch_manager_;
  }

 private:
  // ========== Pipeline Workers ==========
  void DownloadWorker();
  void ApplyWorker();

  // ========== Download Operations ==========
  bool DownloadSinglePatch(PipelinePatchItem& item);
  bool VerifyDownloadedFile(const PipelinePatchItem& item);
  bool CanStartNewDownload() const;
  void MarkDownloadCompleted(PipelinePatchItem& item);

  // ========== Apply Operations ==========
  bool ApplySinglePatch(PipelinePatchItem& item);
  bool ApplyPatchToFile(const PatchFileMetadata& file_info,
                        const std::string& patch_path);
  bool CreateNewFileFromPatch(const PatchFileMetadata& file_info,
                              const std::string& patch_path,
                              const std::string& target_path);
  bool UpdateExistingFile(const PatchFileMetadata& file_info,
                          const std::string& patch_path,
                          const std::string& target_path);
  bool ApplyHDiffPatchInternal(const std::string* source_file,
                               class FileSliceStream& patch_slice,
                               const std::string& output_file,
                               bool is_compressed);
  void MarkApplyCompleted(PipelinePatchItem& item);
  void CleanupAppliedPatch(const PipelinePatchItem& item);

  // ========== Session Management ==========
  std::string GenerateSessionId();
  bool CreateBackup(const std::string& file_path);
  bool RollbackFromBackup(const std::string& file_path);
  void OnPatchApplied(const std::string& patch_name, bool success);
  void CheckAllApplied(const std::string& session_id);
  bool IsAllCompleted() const;

  // ========== Configuration ==========
  PatchServiceConfig patch_service_config_;
  std::shared_ptr<PatchDownloadManager> patch_manager_;
  std::shared_ptr<HttpClient> http_client_;
  std::shared_ptr<PatchManifestProcessor> manifest_;
  std::string package_name_;
  std::string output_dir_;

  // ========== Pipeline State ==========
  PatchDownloadMode mode_;
  int download_threads_;
  int apply_threads_;
  int max_buffer_size_;

  std::vector<PipelinePatchItem> patch_items_;
  mutable std::mutex items_mutex_;

  // Download queue
  std::queue<size_t> download_queue_;
  std::mutex download_queue_mutex_;
  std::condition_variable download_cv_;

  // Apply queue
  std::queue<size_t> apply_queue_;
  std::mutex apply_queue_mutex_;
  std::condition_variable apply_cv_;

  // Worker threads
  std::vector<std::thread> download_workers_;
  std::vector<std::thread> apply_workers_;

  // Counters
  std::atomic<bool> cancelled_{false};
  std::atomic<bool> has_error_{false};
  std::atomic<int> downloaded_count_{0};
  std::atomic<int> applied_count_{0};
  std::atomic<int64_t> downloaded_bytes_{0};
  std::atomic<int> buffer_count_{0};
  int64_t total_bytes_{0};

  // Progress tracking
  std::unique_ptr<ProgressTracker> progress_tracker_;

  // Callbacks
  ProgressCallback progress_callback_;
  PatchAppliedCallback patch_applied_callback_;
  AllPatchesAppliedCallback all_patches_applied_callback_;
  ApplyProgressCallback apply_progress_callback_;

  // Sessions
  struct PatchSessionInfo {
    std::vector<std::string> patch_names;
    std::vector<std::string> backup_files;
    size_t downloaded_count = 0;
    size_t applied_count = 0;
    size_t failed_count = 0;
    std::chrono::steady_clock::time_point start_time;
  };

  mutable std::mutex sessions_mutex_;
  std::unordered_map<std::string, PatchSessionInfo> sessions_;
  std::atomic<uint64_t> session_counter_{0};
};

QUATON_NAMESPACE_END

#endif  // QUATON_PATCH_PATCH_DOWNLOAD_SERVICE_H_
