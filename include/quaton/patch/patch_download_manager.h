#ifndef QUATON_PATCH_PATCH_DOWNLOAD_MANAGER_H_
#define QUATON_PATCH_PATCH_DOWNLOAD_MANAGER_H_

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "quaton/base/download_manager_base.h"
#include "quaton/patch/patch_download_item.h"
#include "quaton/quaton_global.h"

QUATON_NAMESPACE_BEGIN

class HttpClient;
struct PatchInfo;

/**
 * @struct PatchManagerConfig
 * @brief Configuration for patch download manager
 */
struct QUATON_API PatchManagerConfig : public ManagerConfig {
  std::string patch_base_url;  // Base URL for patches
  std::string source_version;  // Source version
  std::string target_version;  // Target version
  std::string source_path;     // Path to source files
  std::string target_path;     // Path to output patched files
  PatchType default_patch_type = PatchType::kLdiff;
  bool delete_patch_after_apply = true;  // Delete patch file after success
  size_t patch_buffer_size = 8 * 1024 * 1024;  // 8MB buffer
};

/**
 * @class PatchDownloadManager
 * @brief Manager for patch-based file updates
 *
 * Coordinates download and apply operations for patches:
 * - Downloads patch files
 * - Applies patches using LDIFF/HPatch
 * - Verifies target files
 */
class QUATON_API PatchDownloadManager : public DownloadManagerBase {
 public:
  /**
   * @brief Constructor
   * @param http_client Shared HTTP client
   */
  explicit PatchDownloadManager(std::shared_ptr<HttpClient> http_client);

  ~PatchDownloadManager() override;

  // ========== Configuration ==========

  /**
   * @brief Initialize with config
   */
  bool Initialize(const PatchManagerConfig& config);

  /**
   * @brief Get current config
   */
  const PatchManagerConfig& GetPatchConfig() const { return patch_config_; }

  // ========== Patch Management ==========

  /**
   * @brief Add patches for download and apply
   * @param patches List of patch info
   * @return Job ID for tracking
   */
  std::string AddPatches(const std::vector<PatchInfo>& patches);

  /**
   * @brief Add a single patch
   */
  std::string AddPatch(const PatchInfo& patch);

  /**
   * @brief Create download item from patch info
   */
  std::shared_ptr<PatchDownloadItem> CreateItemFromPatch(
      const PatchInfo& patch);

  // ========== Operations ==========

  /**
   * @brief Start downloading and applying all patches
   */
  size_t StartAllPatches();

  /**
   * @brief Pause all patch operations
   */
  void PauseAllPatches();

  /**
   * @brief Resume paused patches
   */
  void ResumeAllPatches();

  // ========== Status ==========

  struct PatchStats {
    size_t total_patches = 0;
    size_t downloaded = 0;
    size_t applied = 0;
    size_t failed = 0;
    size_t pending = 0;
    size_t active = 0;
    int64_t total_patch_bytes = 0;
    int64_t downloaded_bytes = 0;
    double download_speed = 0.0;
    double apply_progress = 0.0;
  };

  PatchStats GetPatchStats() const;

  // ========== Item Access ==========

  std::shared_ptr<PatchDownloadItem> GetItemByPatchName(
      const std::string& patch_name) const;

  std::vector<std::shared_ptr<PatchDownloadItem>> GetAllItems() const;

  std::vector<std::shared_ptr<PatchDownloadItem>> GetItemsByState(
      DownloadState state) const;

 protected:
  void OnJobStateChanged(const std::string& job_id,
                         JobState old_state,
                         JobState new_state) override;

  void OnJobProgress(const std::string& job_id,
                     int64_t downloaded,
                     int64_t total) override;

  void OnJobCompleted(const std::string& job_id) override;

  void OnJobFailed(const std::string& job_id,
                   const std::string& error) override;

 private:
  std::string CreatePatchJob(std::shared_ptr<PatchDownloadItem> item);
  bool ApplyPatch(std::shared_ptr<PatchDownloadItem> item);

  PatchManagerConfig patch_config_;
  std::shared_ptr<HttpClient> http_client_;

  mutable std::mutex items_mutex_;
  std::unordered_map<std::string, std::shared_ptr<PatchDownloadItem>> items_;
  std::unordered_map<std::string, std::string> job_to_patch_map_;
};

QUATON_NAMESPACE_END

#endif  // QUATON_PATCH_PATCH_DOWNLOAD_MANAGER_H_
