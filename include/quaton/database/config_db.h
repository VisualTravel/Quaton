#ifndef QUATON_DATABASE_CONFIG_DB_H
#define QUATON_DATABASE_CONFIG_DB_H
#pragma once

#include <sqlite3.h>

#include <string>
#include <vector>

#include "quaton/database/connection.h"
#include "quaton/database/records.h"

namespace Quaton {
namespace Database {

/**
 * @class ConfigDatabase
 * @brief Manages chunk_config.db configuration records
 */
class ConfigDatabase {
 public:
  /**
   * @brief Constructor
   * @param db_path Path to chunk_config.db file
   */
  explicit ConfigDatabase(const std::string& db_path);

  /**
   * @brief Destructor
   */
  ~ConfigDatabase() = default;

  // Disable copy operations
  ConfigDatabase(const ConfigDatabase&) = delete;
  ConfigDatabase& operator=(const ConfigDatabase&) = delete;

  // Enable move operations
  ConfigDatabase(ConfigDatabase&&) noexcept = default;
  ConfigDatabase& operator=(ConfigDatabase&&) noexcept = default;

  /**
   * @brief Initialize database and create tables
   * @return true if initialization succeeds, false otherwise
   */
  bool Initialize();

  /**
   * @brief Insert or update configuration record
   * @param record Configuration record
   * @return true if operation succeeds, false otherwise
   */
  bool UpsertRecord(const ConfigRecord& record);

  /**
   * @brief Query configuration records
   * @param package_id Optional package ID filter (empty for all)
   * @return Vector of configuration records
   */
  std::vector<ConfigRecord> QueryRecords(const std::string& package_id = "");

  /**
   * @brief Get the latest configuration record for a package
   * @param package_id Package ID
   * @return Latest configuration record, empty record if not found
   */
  ConfigRecord GetLatestRecord(const std::string& package_id);

  /**
   * @brief Delete old configuration records, keep specified versions
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

#endif  // QUATON_DATABASE_CONFIG_DB_H
