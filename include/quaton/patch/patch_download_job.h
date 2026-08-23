#ifndef QUATON_PATCH_PATCH_DOWNLOAD_JOB_H_
#define QUATON_PATCH_PATCH_DOWNLOAD_JOB_H_

#include <memory>
#include <string>
#include <unordered_map>

#include "quaton/base/download_job.h"
#include "quaton/patch/patch_download_item.h"
#include "quaton/quaton_global.h"

QUATON_NAMESPACE_BEGIN

class HttpClient;

/**
 * @class PatchDownloadJob
 * @brief Job for downloading and applying patches
 *
 * This job handles the complete lifecycle of patch-based updates:
 * - Downloading patch files from remote server
 * - Verifying patch file integrity
 * - Applying patches to source files (LDIFF/HDiff)
 * - Cleaning up temporary patch files
 *
 * Job workflow:
 * 1. Download - Fetch patch file from server
 * 2. Verify - Check MD5 checksum
 * 3. Apply - Apply patch to source file using appropriate algorithm
 * 4. Cleanup - Remove temporary patch file
 */
class QUATON_API PatchDownloadJob : public DownloadJob {
 public:
  /**
   * @brief Constructor
   * @param job_id Unique job identifier
   * @param http_client Shared HTTP client for downloads
   * @param output_dir Directory containing source files to patch
   * @param patch_dir Directory to store temporary patch files
   */
  PatchDownloadJob(const std::string& job_id,
                   std::shared_ptr<HttpClient> http_client,
                   const std::string& output_dir,
                   const std::string& patch_dir);

  ~PatchDownloadJob() override = default;

  // ========== Configuration ==========

  /**
   * @brief Set base URL for patch downloads
   */
  void SetPatchBaseUrl(const std::string& url) { patch_base_url_ = url; }

  /**
   * @brief Get patch base URL
   */
  const std::string& GetPatchBaseUrl() const { return patch_base_url_; }

  /**
   * @brief Set URL suffix for patch downloads
   */
  void SetPatchUrlSuffix(const std::string& suffix) {
    patch_url_suffix_ = suffix;
  }

  /**
   * @brief Get patch URL suffix
   */
  const std::string& GetPatchUrlSuffix() const { return patch_url_suffix_; }

  /**
   * @brief Set whether to apply patches after download
   */
  void SetApplyEnabled(bool enabled) { apply_enabled_ = enabled; }

  /**
   * @brief Check if apply is enabled
   */
  bool IsApplyEnabled() const { return apply_enabled_; }

  /**
   * @brief Set whether to delete patches after apply
   */
  void SetCleanupEnabled(bool enabled) { cleanup_enabled_ = enabled; }

  /**
   * @brief Check if cleanup is enabled
   */
  bool IsCleanupEnabled() const { return cleanup_enabled_; }

  /**
   * @brief Set whether to verify patches
   */
  void SetVerifyEnabled(bool enabled) { verify_enabled_ = enabled; }

  /**
   * @brief Check if verification is enabled
   */
  bool IsVerifyEnabled() const { return verify_enabled_; }

  /**
   * @brief Set whether to backup original files before patching
   */
  void SetBackupEnabled(bool enabled) { backup_enabled_ = enabled; }

  /**
   * @brief Check if backup is enabled
   */
  bool IsBackupEnabled() const { return backup_enabled_; }

  /**
   * @brief Set output directory (game installation path)
   */
  void SetOutputDir(const std::string& dir) { output_dir_ = dir; }

  /**
   * @brief Get output directory
   */
  const std::string& GetOutputDir() const { return output_dir_; }

  /**
   * @brief Set patch directory (temporary storage)
   */
  void SetPatchDir(const std::string& dir) { patch_dir_ = dir; }

  /**
   * @brief Get patch directory
   */
  const std::string& GetPatchDir() const { return patch_dir_; }

  // ========== Patch-specific Item Management ==========

  /**
   * @brief Add a patch download item
   * @param item Patch download item to add
   */
  void AddPatchItem(std::shared_ptr<PatchDownloadItem> item);

  /**
   * @brief Get all patch items
   */
  std::vector<std::shared_ptr<PatchDownloadItem>> GetPatchItems() const;

  /**
   * @brief Get patch item by ID
   */
  std::shared_ptr<PatchDownloadItem> GetPatchItemById(
      const std::string& item_id) const;

  // ========== Statistics ==========

  /**
   * @brief Get number of patches applied
   */
  size_t GetAppliedCount() const { return applied_count_.load(); }

  /**
   * @brief Get total bytes of patches downloaded
   */
  int64_t GetPatchBytesDownloaded() const {
    return patch_bytes_downloaded_.load();
  }

 protected:
  /**
   * @brief Process a single download item
   * @param item Item to process
   * @return true if processing succeeded
   */
  bool ProcessItem(std::shared_ptr<DownloadItem> item) override;

  /**
   * @brief Called when all items are processed
   */
  void OnAllItemsProcessed() override;

  /**
   * @brief Called when an item completes successfully
   */
  void OnItemCompleted(std::shared_ptr<DownloadItem> item) override;

  /**
   * @brief Called when an item fails
   */
  void OnItemFailed(std::shared_ptr<DownloadItem> item) override;

 private:
  /**
   * @brief Download patch file from server
   * @param item Item to download
   * @return true if download succeeded
   */
  bool DownloadPatch(std::shared_ptr<DownloadItem> item);

  /**
   * @brief Verify downloaded patch file
   * @param item Item to verify
   * @return true if verification succeeded
   */
  bool VerifyPatch(std::shared_ptr<DownloadItem> item);

  /**
   * @brief Apply patch to source file
   * @param item Item to apply
   * @return true if apply succeeded
   */
  bool ApplyPatch(std::shared_ptr<DownloadItem> item);

  /**
   * @brief Apply LDIFF patch format
   */
  bool ApplyLdiffPatch(const std::string& patch_path,
                       const std::string& source_path,
                       const std::string& output_path);

  /**
   * @brief Apply HDiff patch format
   */
  bool ApplyHdiffPatch(const std::string& patch_path,
                       const std::string& source_path,
                       const std::string& output_path);

  /**
   * @brief Backup original file before patching
   * @param file_path Path to file to backup
   * @return Backup file path, or empty on failure
   */
  std::string BackupFile(const std::string& file_path);

  /**
   * @brief Restore file from backup
   * @param original_path Original file path
   * @param backup_path Backup file path
   * @return true if restore succeeded
   */
  bool RestoreFromBackup(const std::string& original_path,
                         const std::string& backup_path);

  /**
   * @brief Cleanup temporary patch file
   * @param item Item to cleanup
   * @return true if cleanup succeeded
   */
  bool CleanupPatch(std::shared_ptr<DownloadItem> item);

  /**
   * @brief Get full download URL for patch
   */
  std::string GetDownloadUrl(std::shared_ptr<DownloadItem> item) const;

  /**
   * @brief Get local path for patch file
   */
  std::string GetPatchPath(std::shared_ptr<DownloadItem> item) const;

  /**
   * @brief Get source file path for patching
   */
  std::string GetSourcePath(std::shared_ptr<DownloadItem> item) const;

  /**
   * @brief Get output file path after patching
   */
  std::string GetOutputPath(std::shared_ptr<DownloadItem> item) const;

  // Configuration
  std::string output_dir_;
  std::string patch_dir_;
  std::string patch_base_url_;
  std::string patch_url_suffix_;
  bool apply_enabled_ = true;
  bool cleanup_enabled_ = true;
  bool verify_enabled_ = true;
  bool backup_enabled_ = false;

  // Statistics
  std::atomic<size_t> applied_count_{0};
  std::atomic<int64_t> patch_bytes_downloaded_{0};

  // Patch items (typed reference for convenience)
  mutable std::mutex patch_items_mutex_;
  std::unordered_map<std::string, std::shared_ptr<PatchDownloadItem>>
      patch_items_;

  // Backup tracking
  std::mutex backup_mutex_;
  std::unordered_map<std::string, std::string> backup_paths_;
};

QUATON_NAMESPACE_END

#endif  // QUATON_PATCH_PATCH_DOWNLOAD_JOB_H_
