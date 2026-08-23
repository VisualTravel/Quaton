#ifndef QUATON_CHUNK_CHUNK_DOWNLOAD_ITEM_H_
#define QUATON_CHUNK_CHUNK_DOWNLOAD_ITEM_H_

#include <memory>
#include <string>
#include <vector>

#include "quaton/base/download_item.h"
#include "quaton/data_chunk.h"
#include "quaton/quaton_global.h"

QUATON_NAMESPACE_BEGIN

/**
 * @class ChunkDownloadItem
 * @brief Download item for chunk-based resources
 *
 * Extends base DownloadItem with chunk-specific properties
 */
class QUATON_API ChunkDownloadItem : public DownloadItem {
 public:
  /**
   * @brief Constructor
   * @param file_name File name/path
   * @param file_size File size in bytes
   * @param file_hash File hash (MD5)
   */
  ChunkDownloadItem(const std::string& file_name,
                    int64_t file_size,
                    const std::string& file_hash);

  // ========== Chunk Properties ==========

  /**
   * @brief Add a data chunk to this item
   */
  void AddChunk(const DataChunk& chunk) { chunks_.push_back(chunk); }

  /**
   * @brief Get all chunks
   */
  const std::vector<DataChunk>& GetChunks() const { return chunks_; }

  /**
   * @brief Get chunk count
   */
  size_t GetChunkCount() const { return chunks_.size(); }

  /**
   * @brief Clear all chunks
   */
  void ClearChunks() { chunks_.clear(); }

  // ========== Chunk Download Info ==========

  /**
   * @brief Get/Set chunk base URL
   */
  const std::string& GetChunkBaseUrl() const { return chunk_base_url_; }
  void SetChunkBaseUrl(const std::string& url) { chunk_base_url_ = url; }

  /**
   * @brief Get/Set whether chunks are compressed
   */
  bool IsCompressed() const { return is_compressed_; }
  void SetCompressed(bool compressed) { is_compressed_ = compressed; }

  // ========== Verification ==========

  /**
   * @brief Get/Set calculated checksum after download
   */
  const std::string& GetCalculatedChecksum() const {
    return calculated_checksum_;
  }
  void SetCalculatedChecksum(const std::string& checksum) {
    calculated_checksum_ = checksum;
  }

  /**
   * @brief Get/Set XXHash checksum
   */
  const std::string& GetXXHashChecksum() const { return xxhash_checksum_; }
  void SetXXHashChecksum(const std::string& checksum) {
    xxhash_checksum_ = checksum;
  }

  // ========== Database Info ==========

  /**
   * @brief Get/Set package ID
   */
  const std::string& GetPackageId() const { return package_id_; }
  void SetPackageId(const std::string& id) { package_id_ = id; }

  /**
   * @brief Get/Set build ID
   */
  const std::string& GetBuildId() const { return build_id_; }
  void SetBuildId(const std::string& id) { build_id_ = id; }

  /**
   * @brief Get/Set depot ID
   */
  const std::string& GetDepotId() const { return depot_id_; }
  void SetDepotId(const std::string& id) { depot_id_ = id; }

  /**
   * @brief Get/Set version
   */
  const std::string& GetVersion() const { return version_; }
  void SetVersion(const std::string& ver) { version_ = ver; }

  // ========== Convenience Methods ==========

  /**
   * @brief Get file name (alias for GetId)
   */
  const std::string& GetFileName() const { return GetId(); }

  /**
   * @brief Get total size (alias for GetExpectedSize)
   */
  int64_t GetTotalSize() const { return GetExpectedSize(); }

  /**
   * @brief Get downloaded size
   */
  int64_t GetDownloadedSize() const { return GetProgress().downloaded_bytes; }

  /**
   * @brief Get/Set destination path
   */
  const std::string& GetDestinationPath() const { return GetFilePath(); }
  void SetDestinationPath(const std::string& path) { SetFilePath(path); }

 protected:
  void OnStateChanged(DownloadState old_state,
                      DownloadState new_state) override;

 private:
  std::vector<DataChunk> chunks_;
  std::string chunk_base_url_;
  bool is_compressed_ = true;
  std::string calculated_checksum_;
  std::string xxhash_checksum_;
  std::string package_id_;
  std::string build_id_;
  std::string depot_id_;
  std::string version_;
};

QUATON_NAMESPACE_END

#endif  // QUATON_CHUNK_CHUNK_DOWNLOAD_ITEM_H_
