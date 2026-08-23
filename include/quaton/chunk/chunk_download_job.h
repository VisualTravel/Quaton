#ifndef QUATON_CHUNK_CHUNK_DOWNLOAD_JOB_H_
#define QUATON_CHUNK_CHUNK_DOWNLOAD_JOB_H_

#include <memory>
#include <string>

#include "quaton/base/download_job.h"
#include "quaton/chunk/chunk_download_item.h"
#include "quaton/quaton_global.h"

QUATON_NAMESPACE_BEGIN

class HttpClient;

/**
 * @class ChunkDownloadJob
 * @brief Job for downloading chunk-based resources
 *
 * This job handles the complete lifecycle of downloading chunk-based files:
 * - Downloading file chunks from remote server
 * - Reassembling chunks into complete files
 * - Verifying file integrity via checksum
 * - Decompressing if needed
 * - Saving download records to database
 *
 * Job workflow:
 * 1. Download - Fetch file data from server
 * 2. Decompress - If chunk is compressed (zstd)
 * 3. Verify - Check MD5/XXHash against manifest
 * 4. Save - Record completion in database
 */
class QUATON_API ChunkDownloadJob : public DownloadJob {
 public:
  /**
   * @brief Constructor
   * @param job_id Unique job identifier
   * @param http_client Shared HTTP client for downloads
   * @param output_dir Directory to save downloaded files
   */
  ChunkDownloadJob(const std::string& job_id,
                   std::shared_ptr<HttpClient> http_client,
                   const std::string& output_dir);

  ~ChunkDownloadJob() override = default;

  // ========== Configuration ==========

  /**
   * @brief Set base URL for chunk downloads
   * @param url Base URL (chunks will be fetched from url + chunk_id)
   */
  void SetChunkBaseUrl(const std::string& url) { chunk_base_url_ = url; }

  /**
   * @brief Get chunk base URL
   */
  const std::string& GetChunkBaseUrl() const { return chunk_base_url_; }

  /**
   * @brief Set whether to verify files after download
   */
  void SetVerifyEnabled(bool enabled) { verify_enabled_ = enabled; }

  /**
   * @brief Check if verification is enabled
   */
  bool IsVerifyEnabled() const { return verify_enabled_; }

  /**
   * @brief Set whether to decompress chunks after download
   */
  void SetDecompressEnabled(bool enabled) { decompress_enabled_ = enabled; }

  /**
   * @brief Check if decompression is enabled
   */
  bool IsDecompressEnabled() const { return decompress_enabled_; }

  /**
   * @brief Set whether to save to database after download
   */
  void SetDatabaseEnabled(bool enabled) { database_enabled_ = enabled; }

  /**
   * @brief Check if database saving is enabled
   */
  bool IsDatabaseEnabled() const { return database_enabled_; }

  /**
   * @brief Set output directory
   */
  void SetOutputDir(const std::string& dir) { output_dir_ = dir; }

  /**
   * @brief Get output directory
   */
  const std::string& GetOutputDir() const { return output_dir_; }

  // ========== Chunk-specific Item Management ==========

  /**
   * @brief Add a chunk download item
   * @param item Chunk download item to add
   */
  void AddChunkItem(std::shared_ptr<ChunkDownloadItem> item);

  /**
   * @brief Get all chunk items
   */
  std::vector<std::shared_ptr<ChunkDownloadItem>> GetChunkItems() const;

  /**
   * @brief Get chunk item by ID
   */
  std::shared_ptr<ChunkDownloadItem> GetChunkItemById(
      const std::string& item_id) const;

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
   * @brief Download file from server
   * @param item Item to download
   * @return true if download succeeded
   */
  bool DownloadFile(std::shared_ptr<DownloadItem> item);

  /**
   * @brief Decompress downloaded chunk
   * @param item Item to decompress
   * @return true if decompression succeeded
   */
  bool DecompressChunk(std::shared_ptr<DownloadItem> item);

  /**
   * @brief Verify downloaded file
   * @param item Item to verify
   * @return true if verification succeeded
   */
  bool VerifyFile(std::shared_ptr<DownloadItem> item);

  /**
   * @brief Save download record to database
   * @param item Item to save
   * @return true if save succeeded
   */
  bool SaveToDatabase(std::shared_ptr<DownloadItem> item);

  /**
   * @brief Get full download URL for item
   */
  std::string GetDownloadUrl(std::shared_ptr<DownloadItem> item) const;

  /**
   * @brief Get output file path for item
   */
  std::string GetOutputPath(std::shared_ptr<DownloadItem> item) const;

  // Configuration
  std::string output_dir_;
  std::string chunk_base_url_;
  bool verify_enabled_ = true;
  bool decompress_enabled_ = true;
  bool database_enabled_ = true;

  // Chunk items (typed reference for convenience)
  mutable std::mutex chunk_items_mutex_;
  std::unordered_map<std::string, std::shared_ptr<ChunkDownloadItem>>
      chunk_items_;
};

QUATON_NAMESPACE_END

#endif  // QUATON_CHUNK_CHUNK_DOWNLOAD_JOB_H_
