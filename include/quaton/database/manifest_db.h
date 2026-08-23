#ifndef QUATON_DATABASE_MANIFEST_DB_H
#define QUATON_DATABASE_MANIFEST_DB_H
#pragma once

#include <sqlite3.h>

#include <string>
#include <vector>

#include "quaton/database/connection.h"
#include "quaton/database/records.h"

namespace Quaton {
namespace Database {

/**
 * @class ManifestDatabase
 * @brief Manages chunk_manifest.db manifest records
 */
class ManifestDatabase {
 public:
  /**
   * @brief Constructor
   * @param db_path Path to chunk_manifest.db file
   */
  explicit ManifestDatabase(const std::string& db_path);

  /**
   * @brief Destructor
   */
  ~ManifestDatabase() = default;

  // Disable copy operations
  ManifestDatabase(const ManifestDatabase&) = delete;
  ManifestDatabase& operator=(const ManifestDatabase&) = delete;

  // Enable move operations
  ManifestDatabase(ManifestDatabase&&) noexcept = default;
  ManifestDatabase& operator=(ManifestDatabase&&) noexcept = default;

  /**
   * @brief Initialize database and create tables
   * @return true if initialization succeeds, false otherwise
   */
  bool Initialize();

  /**
   * @brief Insert a single manifest record
   * @param record Manifest record to insert
   * @return true if insertion succeeds, false otherwise
   */
  bool InsertRecord(const ManifestRecord& record);

  /**
   * @brief Insert multiple manifest records
   * @param records Vector of manifest records to insert
   * @return true if all insertions succeed, false otherwise
   */
  bool InsertRecords(const std::vector<ManifestRecord>& records);

  /**
   * @brief Query manifest records by package ID and optional build ID
   * @param package_id Package ID to query
   * @param build_id Optional build ID filter (empty for all builds)
   * @return Vector of matching manifest records
   */
  std::vector<ManifestRecord> QueryRecords(const std::string& package_id,
                                           const std::string& build_id = "");

  /**
   * @brief Delete old manifest records, keep specified versions
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

#endif  // QUATON_DATABASE_MANIFEST_DB_H
