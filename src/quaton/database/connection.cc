#include "quaton/database/connection.h"

#include <filesystem>
#include <sstream>
#include <vector>

#include "quaton/logger.h"

namespace fs = std::filesystem;

namespace Quaton {
namespace Database {

Connection::Connection(const DatabaseConfig& config)
    : db_path_(config.get_full_path()), db_(nullptr), config_(config) {
}

Connection::Connection(const std::string& db_path)
    : db_path_(db_path),
      db_(nullptr),
      config_(DatabaseConfig::create_default()) {
}

Connection::~Connection() {
  Close();
}

Connection::Connection(Connection&& other) noexcept
    : db_path_(std::move(other.db_path_)), db_(other.db_) {
  other.db_ = nullptr;
}

Connection& Connection::operator=(Connection&& other) noexcept {
  if (this != &other) {
    Close();
    db_path_ = std::move(other.db_path_);
    db_ = other.db_;
    other.db_ = nullptr;
  }
  return *this;
}

bool Connection::Open() {
  if (IsOpen()) {
    LOG_WARN("Database already open: %s", db_path_.c_str());
    return true;
  }

  // Ensure parent directory exists
  fs::path path(db_path_);
  if (path.has_parent_path()) {
    fs::path parent = path.parent_path();
    if (!fs::exists(parent)) {
      std::error_code ec;
      if (!fs::create_directories(parent, ec)) {
        LOG_ERROR("Failed to create database directory: %s - %s",
                  parent.string().c_str(),
                  ec.message().c_str());
        return false;
      }
    }
  }

  int rc = sqlite3_open(db_path_.c_str(), &db_);
  if (rc != SQLITE_OK) {
    LOG_ERROR("Failed to open database %s: %s",
              db_path_.c_str(),
              sqlite3_errmsg(db_));
    if (db_) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
    return false;
  }

  // Apply settings from config
  if (!InitializeDatabase(db_, config_)) {
    LOG_ERROR("Failed to initialize database settings: %s", db_path_.c_str());
    Close();
    return false;
  }

  LOG_INFO("Opened database: %s", db_path_.c_str());
  return true;
}

void Connection::Close() {
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
    LOG_INFO("Closed database: %s", db_path_.c_str());
  }
}

bool Connection::Execute(const std::string& sql, std::string& error_msg) {
  if (!IsOpen()) {
    error_msg = "Database not open";
    return false;
  }

  char* err_msg = nullptr;
  int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err_msg);

  if (rc != SQLITE_OK) {
    error_msg = err_msg ? err_msg : "Unknown error";
    if (err_msg) {
      sqlite3_free(err_msg);
    }
    LOG_ERROR("SQL execution failed: %s - %s", error_msg.c_str(), sql.c_str());
    return false;
  }

  return true;
}

bool InitializeDatabase(sqlite3* db, const DatabaseConfig& config) {
  if (!db) {
    return false;
  }

  // Build pragmas based on config
  std::vector<std::string> pragmas;

  if (config.enable_wal_mode) {
    pragmas.push_back("PRAGMA journal_mode = WAL;");
  }
  pragmas.push_back("PRAGMA synchronous = NORMAL;");

  // Cache size in KB (negative means KB, positive means pages)
  std::ostringstream cache_pragma;
  cache_pragma << "PRAGMA cache_size = -" << config.cache_size_kb << ";";
  pragmas.push_back(cache_pragma.str());

  pragmas.push_back("PRAGMA temp_store = MEMORY;");

  if (config.enable_foreign_keys) {
    pragmas.push_back("PRAGMA foreign_keys = ON;");
  }

  // Busy timeout
  std::ostringstream timeout_pragma;
  timeout_pragma << "PRAGMA busy_timeout = " << config.busy_timeout_ms << ";";
  pragmas.push_back(timeout_pragma.str());

  for (const auto& pragma : pragmas) {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(db, pragma.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
      LOG_ERROR("Failed to execute pragma: %s - %s",
                pragma.c_str(),
                err_msg ? err_msg : "Unknown error");
      if (err_msg) {
        sqlite3_free(err_msg);
      }
      return false;
    }
  }

  return true;
}

bool InitializeDatabase(sqlite3* db) {
  return InitializeDatabase(db, DatabaseConfig::create_default());
}

}  // namespace Database
}  // namespace Quaton
