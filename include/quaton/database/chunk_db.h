#ifndef QUATON_DATABASE_CHUNK_DB_H
#define QUATON_DATABASE_CHUNK_DB_H
#pragma once

#include <sqlite3.h>

#include <memory>
#include <string>
#include <vector>

#include "quaton/database/connection.h"
#include "quaton/database/records.h"

namespace Quaton {
namespace Database {

/**
 * @class ChunkDatabase
 * @brief Manages chunk.db file records
 */
class ChunkDatabase {
 public:
  /**
   * @brief Constructor
   * @param db_path Path to chunk.db file
   */
  explicit ChunkDatabase(const std::string& db_path);

  /**
   * @brief Destructor
   */
  ~ChunkDatabase() = default;

  // Disable copy operations
  ChunkDatabase(const ChunkDatabase&) = delete;
  ChunkDatabase& operator=(const ChunkDatabase&) = delete;

  // Enable move operations
  ChunkDatabase(ChunkDatabase&&) noexcept = default;
  ChunkDatabase& operator=(ChunkDatabase&&) noexcept = default;

  /**
   * @brief Initialize database and create tables
   * @return true if initialization succeeds, false otherwise
   */
  bool Initialize();

  /**
   * @brief Insert single file record
   * @param record File record to insert
   * @return true if insertion succeeds, false otherwise
   */
  bool InsertRecord(const FileRecord& record);

  /**
   * @brief Insert multiple file records in transaction
   * @param records Vector of file records to insert
   * @return true if all insertions succeed, false otherwise
   */
  bool InsertRecords(const std::vector<FileRecord>& records);

  /**
   * @brief Query file records by package ID and optional file ID
   * @param package_id Package ID to query
   * @param file_id Optional file ID filter (empty for all files)
   * @return Vector of matching file records
   */
  std::vector<FileRecord> QueryRecords(const std::string& package_id,
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
   * @param params File validation parameters
   * @return true if file exists and matches database record, false otherwise
   */
  bool IsFileValid(const FileValidationParams& params);

  /**
   * @brief Delete old file records, keep specified versions
   * @param package_id Package ID
   * @param keep_versions Number of versions to keep (default 2)
   * @return true if deletion succeeds, false otherwise
   */
  bool DeleteOldRecords(const std::string& package_id, int keep_versions = 2);

  /**
   * @brief Get database connection
   * @return Database connection reference
   */
  Connection& GetConnection() { return conn_; }

 private:
  /**
   * @brief Create database tables
   * @return true if table creation succeeds, false otherwise
   */
  bool CreateTables();

  Connection conn_;  ///< Database connection
};

}  // namespace Database
}  // namespace Quaton

#endif  // QUATON_DATABASE_CHUNK_DB_H
