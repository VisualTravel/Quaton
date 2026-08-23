#ifndef QUATON_CHUNK_CHUNK_DOWNLOAD_MANAGER_H_
#define QUATON_CHUNK_CHUNK_DOWNLOAD_MANAGER_H_

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "quaton/base/download_manager_base.h"
#include "quaton/chunk/chunk_download_item.h"
#include "quaton/quaton_global.h"
#include "quaton/resource.h"

QUATON_NAMESPACE_BEGIN

class HttpClient;

/**
 * @struct ChunkManagerConfig
 * @brief Configuration for chunk download manager
 */
struct QUATON_API ChunkManagerConfig : public ManagerConfig {
  std::string chunk_base_url;     // Base URL for chunks
  std::string package_id;         // Package identifier
  std::string build_id;           // Build identifier
  std::string version;            // Version string
  bool decompress_chunks = true;  // Decompress chunks after download
  size_t chunk_buffer_size = 4 * 1024 * 1024;  // 4MB buffer
};

/**
 * @class ChunkDownloadManager
 * @brief Manager for chunk-based file downloads
 *
 * Coordinates download operations for chunk-based resources:
 * - Creates ChunkDownloadItems from Resource objects
 * - Submits ChunkDownloadJobs to scheduler
 * - Handles verification and decompression
 */
class QUATON_API ChunkDownloadManager : public DownloadManagerBase {
 public:
  /**
   * @brief Constructor
   * @param http_client Shared HTTP client
   */
  explicit ChunkDownloadManager(std::shared_ptr<HttpClient> http_client);

  ~ChunkDownloadManager() override;

  // ========== Configuration ==========

  /**
   * @brief Initialize with config
   */
  bool Initialize(const ChunkManagerConfig& config);

  /**
   * @brief Get current config
   */
  const ChunkManagerConfig& GetChunkConfig() const { return chunk_config_; }

  // ========== Resource Management ==========

  /**
   * @brief Add resources for download
   * @param resources List of resources to download
   * @return Job ID for tracking
   */
  std::string AddResources(const std::vector<Resource>& resources);

  /**
   * @brief Add a single resource
   * @param resource Resource to download
   * @return Job ID
   */
  std::string AddResource(const Resource& resource);

  /**
   * @brief Create download item from resource
   */
  std::shared_ptr<ChunkDownloadItem> CreateItemFromResource(
      const Resource& resource);

  // ========== Download Operations ==========

  /**
   * @brief Start downloading all pending items
   * @return Number of jobs started
   */
  size_t StartAllDownloads();

  /**
   * @brief Pause all active downloads
   */
  void PauseAllDownloads();

  /**
   * @brief Resume paused downloads
   */
  void ResumeAllDownloads();

  // ========== Status ==========

  /**
   * @brief Get download statistics
   */
  struct ChunkDownloadStats {
    size_t total_files = 0;
    size_t completed_files = 0;
    size_t failed_files = 0;
    size_t pending_files = 0;
    size_t active_files = 0;
    int64_t total_bytes = 0;
    int64_t downloaded_bytes = 0;
    double download_speed = 0.0;
  };

  ChunkDownloadStats GetDownloadStats() const;

  // ========== Item Access ==========

  /**
   * @brief Get download item by file name
   */
  std::shared_ptr<ChunkDownloadItem> GetItemByFileName(
      const std::string& file_name) const;

  /**
   * @brief Get all download items
   */
  std::vector<std::shared_ptr<ChunkDownloadItem>> GetAllItems() const;

  /**
   * @brief Get items by state
   */
  std::vector<std::shared_ptr<ChunkDownloadItem>> GetItemsByState(
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
  std::string CreateChunkJob(std::shared_ptr<ChunkDownloadItem> item);

  ChunkManagerConfig chunk_config_;
  std::shared_ptr<HttpClient> http_client_;

  // File name -> Item mapping
  mutable std::mutex items_mutex_;
  std::unordered_map<std::string, std::shared_ptr<ChunkDownloadItem>> items_;

  // Job ID -> File name mapping for reverse lookup
  std::unordered_map<std::string, std::string> job_to_file_map_;
};

QUATON_NAMESPACE_END

#endif  // QUATON_CHUNK_CHUNK_DOWNLOAD_MANAGER_H_
