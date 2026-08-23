#ifndef DATABASE_MANAGER_H
#define DATABASE_MANAGER_H
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "quaton/configuration/database_config.h"
#include "quaton/database/chunk_db.h"
#include "quaton/database/config_db.h"
#include "quaton/database/manifest_db.h"
#include "quaton/database/records.h"
#include "quaton/quaton_global.h"  // For QUATON_API macro

namespace Quaton {

/**
 * @class DatabaseManager
 * @brief Database management facade for chunk, configuration, and manifest
 * databases
 *
 * Provides a unified interface to three SQLite databases:
 * - chunk.db: File records
 * - chunk_config.db: Configuration records
 * - chunk_manifest.db: Manifest records
 */
class QUATON_API DatabaseManager {
 public:
  /**
   * @brief Get DatabaseManager singleton instance
   * @return Reference to DatabaseManager instance
   * @throws std::runtime_error if initialization fails
   */
  static DatabaseManager& Instance();

  /**
   * @brief Destructor
   */
  ~DatabaseManager() = default;

  // Disable copy and move operations for singleton
  DatabaseManager(const DatabaseManager&) = delete;
  DatabaseManager& operator=(const DatabaseManager&) = delete;
  DatabaseManager(DatabaseManager&&) = delete;
  DatabaseManager& operator=(DatabaseManager&&) = delete;

  // ==================== chunk.db operations ====================

  /**
   * @brief Insert single file record
   * @param record File record to insert
   * @return true if insertion succeeds, false otherwise
   * @throws std::invalid_argument if record fields are invalid
   */
  bool InsertFileRecord(const FileRecord& record);

  /**
   * @brief Insert multiple file records in transaction
   * @param records Vector of file records to insert
   * @return true if all insertions succeed, false otherwise
   */
  bool InsertFileRecords(const std::vector<FileRecord>& records);

  /**
   * @brief Query file records by package ID and optional file ID
   * @param package_id Package ID to query
   * @param file_id Optional file ID filter (empty for all files)
   * @return Vector of matching file records
   */
  std::vector<FileRecord> QueryFileRecords(const std::string& package_id,
                                           const std::string& file_id = "");

  /**
   * @brief Update file status
   * @param package_id Package ID
   * @param branch Branch name
   * @param file_id File ID
   * @param status New status value
   * @return true if update succeeds, false otherwise
   */
  bool UpdateFileStatus(const std::string& package_id,
                        const std::string& branch,
                        const std::string& file_id,
                        int status);

  /**
   * @brief Check if file is valid (exists and checksum matches database record)
   * @param params File validation parameters containing file path, install dir,
   *               checksum, and version identifiers
   * @return true if file exists and matches database record, false otherwise
   * @note The checksum is used for database lookup, not direct hash comparison
   */
  bool IsFileValid(const FileValidationParams& params);

  /**
   * @brief Delete old file records, keep specified versions
   * @param package_id Package ID
   * @param keep_versions Number of versions to keep (default 2)
   * @return true if deletion succeeds, false otherwise
   */
  bool DeleteOldFileRecords(const std::string& package_id,
                            int keep_versions = 2);

  // ==================== chunk_config.db operations ====================

  /**
   * @brief Insert or update configuration record
   * @param record Configuration record
   * @return true if operation succeeds, false otherwise
   * @throws std::invalid_argument if record fields are invalid
   */
  bool UpsertConfigRecord(const ConfigRecord& record);

  /**
   * @brief Query configuration records
   * @param package_id Optional package ID filter (empty for all)
   * @return Vector of configuration records
   */
  std::vector<ConfigRecord> QueryConfigRecords(
      const std::string& package_id = "");

  /**
   * @brief Get the latest configuration record for a package
   * @param package_id Package ID
   * @return Latest configuration record, empty record if not found
   */
  ConfigRecord GetLatestConfig(const std::string& package_id);

  /**
   * @brief Delete old configuration records, keep specified versions
   * @param package_id Package ID
   * @param keep_versions Number of versions to keep (default 2)
   * @return true if deletion succeeds, false otherwise
   */
  bool DeleteOldConfigRecords(const std::string& package_id,
                              int keep_versions = 2);

  // ==================== chunk_manifest.db operations ====================

  /**
   * @brief Insert a single manifest record
   * @param record Manifest record to insert
   * @return true if insertion succeeds, false otherwise
   * @throws std::invalid_argument if record fields are invalid
   */
  bool InsertManifestRecord(const ManifestRecord& record);

  /**
   * @brief Insert multiple manifest records
   * @param records Vector of manifest records to insert
   * @return true if all insertions succeed, false otherwise
   */
  bool InsertManifestRecords(const std::vector<ManifestRecord>& records);

  /**
   * @brief Query manifest records by package ID and optional build ID
   * @param package_id Package ID to query
   * @param build_id Optional build ID filter (empty for all builds)
   * @return Vector of matching manifest records
   */
  std::vector<ManifestRecord> QueryManifestRecords(
      const std::string& package_id, const std::string& build_id = "");

  /**
   * @brief Delete old manifest records, keep specified versions
   * @param package_id Package ID
   * @param keep_versions Number of versions to keep (default 2)
   * @return true if deletion succeeds, false otherwise
   */
  bool DeleteOldManifestRecords(const std::string& package_id,
                                int keep_versions = 2);

  // ==================== Exported functions ====================

  /**
   * @brief Update chunk configuration (wrapper for upsert_config_record)
   * @param record Configuration record
   * @return true if update succeeds, false otherwise
   */
  bool UpdateChunkConfig(const ConfigRecord& record);

 private:
  // Allow std::make_unique to access private constructor for singleton pattern
  friend std::unique_ptr<DatabaseManager>
  std::make_unique<DatabaseManager, std::string&>(std::string&);

  /**
   * @brief Private constructor for singleton pattern
   * @param db_dir Database directory path
   * @throws std::invalid_argument if db_dir is empty
   */
  explicit DatabaseManager(const std::string& db_dir);

  /**
   * @brief Initialize all database connections and tables
   * @return true if initialization succeeds, false otherwise
   */
  bool Initialize();

  std::string db_dir_;  ///< Database directory path
  std::unique_ptr<Database::ChunkDatabase> chunk_db_;    ///< Chunk database
  std::unique_ptr<Database::ConfigDatabase> config_db_;  ///< Config database
  std::unique_ptr<Database::ManifestDatabase>
      manifest_db_;  ///< Manifest database
  // Worker threads share this singleton; sqlite connections must not be used
  // concurrently. Recursive because UpdateChunkConfig wraps UpsertConfigRecord.
  mutable std::recursive_mutex db_mutex_;
};

}  // namespace Quaton

#endif  // DATABASE_MANAGER_H
