#ifndef QUATON_DATABASE_CONNECTION_H
#define QUATON_DATABASE_CONNECTION_H
#pragma once

#include <sqlite3.h>

#include <memory>
#include <string>

#include "quaton/configuration/database_config.h"

namespace Quaton {
namespace Database {

/**
 * @class Connection
 * @brief SQLite database connection wrapper with RAII
 */
class Connection {
 public:
  /**
   * @brief Constructor with DatabaseConfig
   * @param config Database configuration
   */
  explicit Connection(const DatabaseConfig& config);

  /**
   * @brief Constructor with database path
   * @param db_path Full path to database file
   */
  explicit Connection(const std::string& db_path);

  /**
   * @brief Destructor - automatically closes connection
   */
  ~Connection();

  // Disable copy operations
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  // Enable move operations
  Connection(Connection&& other) noexcept;
  Connection& operator=(Connection&& other) noexcept;

  /**
   * @brief Open database connection
   * @return true if connection succeeds, false otherwise
   */
  bool Open();

  /**
   * @brief Close database connection
   */
  void Close();

  /**
   * @brief Check if database is open
   * @return true if database is open, false otherwise
   */
  bool IsOpen() const { return db_ != nullptr; }

  /**
   * @brief Get raw SQLite database handle
   * @return Raw sqlite3 pointer
   */
  sqlite3* GetHandle() const { return db_; }

  /**
   * @brief Execute SQL statement
   * @param sql SQL statement string
   * @param error_msg Output error message if execution fails
   * @return true if execution succeeds, false otherwise
   */
  bool Execute(const std::string& sql, std::string& error_msg);

  /**
   * @brief Get database path
   * @return Database file path
   */
  const std::string& GetPath() const { return db_path_; }

 private:
  std::string db_path_;    ///< Database file path
  sqlite3* db_;            ///< SQLite database handle
  DatabaseConfig config_;  ///< Database configuration
};

/**
 * @brief Initialize database with configuration settings
 * @param db SQLite database handle
 * @param config Database configuration
 * @return true if initialization succeeds, false otherwise
 */
bool InitializeDatabase(sqlite3* db, const DatabaseConfig& config);

/**
 * @brief Initialize database with default settings
 * @param db SQLite database handle
 * @return true if initialization succeeds, false otherwise
 */
bool InitializeDatabase(sqlite3* db);

}  // namespace Database
}  // namespace Quaton

#endif  // QUATON_DATABASE_CONNECTION_H
