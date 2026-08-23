#ifndef QUATON_CONFIGURATION_DATABASE_CONFIG_H
#define QUATON_CONFIGURATION_DATABASE_CONFIG_H
#pragma once

#include <filesystem>
#include <string>

namespace Quaton {

/**
 * @struct DatabaseConfig
 * @brief Database configuration settings
 */
struct DatabaseConfig {
  std::string db_directory;               ///< Database directory path
  std::string db_filename = "quaton.db";  ///< Database filename
  int busy_timeout_ms = 5000;       ///< SQLite busy timeout in milliseconds
  bool enable_wal_mode = true;      ///< Enable Write-Ahead Logging
  bool enable_foreign_keys = true;  ///< Enable foreign key constraints
  int cache_size_kb = 2000;         ///< Cache size in KB

  /**
   * @brief Get full database path using std::filesystem::path
   * @return Full path to database file
   */
  std::string get_full_path() const {
    if (db_directory.empty()) {
      return db_filename;
    }
    std::filesystem::path dir(db_directory);
    return (dir / db_filename).string();
  }

  /**
   * @brief Create default database configuration
   * @return Default database configuration
   */
  static DatabaseConfig create_default() { return DatabaseConfig{}; }

  /**
   * @brief Create database configuration with custom directory
   * @param directory Database directory path
   * @return Database configuration with custom directory
   */
  static DatabaseConfig with_directory(const std::string& directory) {
    DatabaseConfig config;
    config.db_directory = directory;
    return config;
  }

  /**
   * @brief Create database configuration with custom directory and filename
   * @param directory Database directory path
   * @param filename Database filename
   * @return Database configuration with custom directory and filename
   */
  static DatabaseConfig with_path(const std::string& directory,
                                  const std::string& filename) {
    DatabaseConfig config;
    config.db_directory = directory;
    config.db_filename = filename;
    return config;
  }
};

}  // namespace Quaton

#endif  // QUATON_CONFIGURATION_DATABASE_CONFIG_H
