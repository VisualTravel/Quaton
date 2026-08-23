#include "quaton/database/chunk_db.h"

#include <filesystem>
#include <stdexcept>

#include "quaton/database/transaction.h"
#include "quaton/logger.h"
#include "quaton/utils/checksum.h"

namespace fs = std::filesystem;

namespace Quaton {
namespace Database {

ChunkDatabase::ChunkDatabase(const std::string& db_path) : conn_(db_path) {
}

bool ChunkDatabase::Initialize() {
  if (!conn_.Open()) {
    return false;
  }

  return CreateTables();
}

bool ChunkDatabase::CreateTables() {
  std::string create_table_sql = R"(
        CREATE TABLE IF NOT EXISTS files (
            branch TEXT NOT NULL,
            package_id TEXT NOT NULL,
            build_id TEXT NOT NULL,
            depot_id TEXT NOT NULL,
            file_id TEXT NOT NULL,
            file_ver TEXT NOT NULL,
            install_dir TEXT NOT NULL,
            file_checksum TEXT NOT NULL,
            file_status INTEGER NOT NULL DEFAULT 0,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            PRIMARY KEY(branch, package_id, build_id, file_id)
        );
        CREATE INDEX IF NOT EXISTS idx_files_package ON files(package_id);
        CREATE INDEX IF NOT EXISTS idx_files_branch ON files(branch);
        CREATE INDEX IF NOT EXISTS idx_files_branch_package_build ON files(branch, package_id, build_id);
        CREATE INDEX IF NOT EXISTS idx_files_build ON files(build_id);
        CREATE INDEX IF NOT EXISTS idx_files_file ON files(file_id);
        CREATE INDEX IF NOT EXISTS idx_files_checksum ON files(file_checksum);
    )";

  std::string error_msg;
  if (!conn_.Execute(create_table_sql, error_msg)) {
    LOG_ERROR("Failed to create chunk table: %s", error_msg.c_str());
    return false;
  }

  return true;
}

bool ChunkDatabase::InsertRecord(const FileRecord& record) {
  if (record.package_id.empty() || record.branch.empty() ||
      record.build_id.empty() || record.file_id.empty()) {
    throw std::invalid_argument(
        "Package ID, branch, build ID, and file ID cannot be empty");
  }
  if (record.file_checksum.empty()) {
    throw std::invalid_argument("File checksum cannot be empty");
  }

  const char* sql = R"(
        INSERT OR REPLACE INTO files 
        (branch, package_id, build_id, depot_id, file_id, file_ver, install_dir, file_checksum, file_status)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(conn_.GetHandle(), sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    LOG_ERROR("Failed to prepare insert statement: %s",
              sqlite3_errmsg(conn_.GetHandle()));
    return false;
  }

  auto stmt_guard = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
      stmt, sqlite3_finalize);

  sqlite3_bind_text(stmt, 1, record.branch.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, record.package_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, record.build_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, record.depot_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, record.file_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, record.file_ver.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 7, record.install_dir.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(
      stmt, 8, record.file_checksum.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 9, record.file_status);

  rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    LOG_ERROR("Failed to insert file record: %s",
              sqlite3_errmsg(conn_.GetHandle()));
    return false;
  }

  return true;
}

bool ChunkDatabase::InsertRecords(const std::vector<FileRecord>& records) {
  if (records.empty()) {
    LOG_DEBUG("No records to insert");
    return true;
  }

  return ExecuteInTransaction(conn_.GetHandle(), [&]() {
    for (const auto& record : records) {
      try {
        if (!InsertRecord(record)) {
          LOG_ERROR("Failed to insert record for file: %s",
                    record.file_id.c_str());
          return false;
        }
      } catch (const std::exception& e) {
        LOG_ERROR("Exception during bulk insert: %s", e.what());
        return false;
      }
    }
    return true;
  });
}

std::vector<FileRecord> ChunkDatabase::QueryRecords(
    const std::string& package_id, const std::string& file_id) {
  if (package_id.empty()) {
    throw std::invalid_argument("Package ID cannot be empty");
  }

  std::vector<FileRecord> records;

  std::string sql =
      "SELECT branch, package_id, build_id, depot_id, file_id, file_ver, "
      "install_dir, file_checksum, file_status FROM files WHERE package_id = ?";
  if (!file_id.empty()) {
    sql += " AND file_id = ?";
  }
  sql += " ORDER BY created_at DESC;";

  sqlite3_stmt* stmt = nullptr;
  int rc =
      sqlite3_prepare_v2(conn_.GetHandle(), sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    LOG_ERROR("Failed to prepare query: %s", sqlite3_errmsg(conn_.GetHandle()));
    return records;
  }

  auto stmt_guard = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
      stmt, sqlite3_finalize);

  sqlite3_bind_text(stmt, 1, package_id.c_str(), -1, SQLITE_TRANSIENT);
  if (!file_id.empty()) {
    sqlite3_bind_text(stmt, 2, file_id.c_str(), -1, SQLITE_TRANSIENT);
  }

  records.reserve(100);

  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    FileRecord record;
    record.branch = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    record.package_id =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    record.build_id =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    record.depot_id =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    record.file_id =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    record.file_ver =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    record.install_dir =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    record.file_checksum =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
    record.file_status = sqlite3_column_int(stmt, 8);
    records.push_back(std::move(record));
  }

  if (rc != SQLITE_DONE) {
    LOG_ERROR("Query execution failed: %s", sqlite3_errmsg(conn_.GetHandle()));
    records.clear();
  }

  return records;
}

bool ChunkDatabase::UpdateFileStatus(const std::string& package_id,
                                     const std::string& branch,
                                     const std::string& file_id,
                                     int status) {
  if (package_id.empty() || branch.empty() || file_id.empty()) {
    throw std::invalid_argument(
        "Package ID, branch, and file ID cannot be empty");
  }

  const char* sql =
      "UPDATE files SET file_status = ? WHERE package_id = ? AND branch = ? "
      "AND file_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(conn_.GetHandle(), sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    LOG_ERROR("Failed to prepare update: %s",
              sqlite3_errmsg(conn_.GetHandle()));
    return false;
  }

  auto stmt_guard = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
      stmt, sqlite3_finalize);

  sqlite3_bind_int(stmt, 1, status);
  sqlite3_bind_text(stmt, 2, package_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, branch.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, file_id.c_str(), -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    LOG_ERROR("Failed to update file status: %s",
              sqlite3_errmsg(conn_.GetHandle()));
    return false;
  }

  return true;
}

bool ChunkDatabase::DeleteOldRecords(const std::string& package_id,
                                     int keep_versions) {
  if (package_id.empty()) {
    throw std::invalid_argument("Package ID cannot be empty");
  }
  if (keep_versions < 1) {
    throw std::invalid_argument("Keep versions must be at least 1");
  }

  const char* sql = R"(
        DELETE FROM files
        WHERE package_id = ?
        AND file_ver NOT IN (
            SELECT DISTINCT file_ver FROM files
            WHERE package_id = ?
            ORDER BY created_at DESC
            LIMIT ?
        );
    )";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(conn_.GetHandle(), sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    LOG_ERROR("Failed to prepare delete: %s",
              sqlite3_errmsg(conn_.GetHandle()));
    return false;
  }

  auto stmt_guard = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
      stmt, sqlite3_finalize);

  sqlite3_bind_text(stmt, 1, package_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, package_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 3, keep_versions);

  rc = sqlite3_step(stmt);
  int deleted = sqlite3_changes(conn_.GetHandle());
  if (rc != SQLITE_DONE) {
    LOG_ERROR("Failed to delete old records: %s",
              sqlite3_errmsg(conn_.GetHandle()));
    return false;
  }

  if (deleted > 0) {
    LOG_INFO("Deleted %d old file records for package %s",
             deleted,
             package_id.c_str());
  }

  return true;
}

bool ChunkDatabase::IsFileValid(const FileValidationParams& params) {
  if (params.file_path.empty() || params.install_dir.empty()) {
    throw std::invalid_argument(
        "File path and install directory cannot be empty");
  }

  // Construct full file path
  fs::path full_path = fs::path(params.install_dir) / params.file_path;

  LOG_DEBUG("Checking file validity: %s", full_path.string().c_str());

  // First check if file exists
  bool file_exists = fs::exists(full_path);
  LOG_DEBUG("File exists check: %s -> %s",
            full_path.string().c_str(),
            file_exists ? "true" : "false");

  if (!file_exists) {
    LOG_DEBUG("File does not exist, marking as invalid: %s",
              full_path.string().c_str());
    return false;
  }

  // Calculate actual file's XXHash for validation
  std::string calculated_xxhash =
      ChecksumUtils::calculate_xxhash_file(full_path.string());
  LOG_DEBUG("Calculated XXHash for %s: %s",
            full_path.string().c_str(),
            calculated_xxhash.c_str());

  if (calculated_xxhash.empty()) {
    LOG_ERROR("Failed to calculate XXHash for file validation: %s",
              full_path.string().c_str());
    return false;
  }

  LOG_DEBUG(
      "Performing database validation (stored checksum=%s, calculated=%s)",
      params.file_checksum.c_str(),
      calculated_xxhash.c_str());

  // Check database connection
  if (!conn_.IsOpen()) {
    LOG_ERROR("Database not initialized for is_file_valid check");
    return false;
  }

  // Query database to verify the file record
  const char* sql = R"(
        SELECT COUNT(*)
        FROM files
        WHERE branch = ? AND package_id = ? AND build_id = ? AND depot_id = ? AND file_id = ?
              AND file_ver = ? AND install_dir = ? AND file_checksum = ? AND file_status = 1
        ORDER BY created_at DESC
        LIMIT 1;
    )";

  LOG_DEBUG(
      "Querying database for file validation: branch=%s, package_id=%s, "
      "build_id=%s, depot_id=%s, file_id=%s, file_ver=%s, install_dir=%s, "
      "checksum=%s",
      params.branch.c_str(),
      params.package_id.c_str(),
      params.build_id.c_str(),
      params.depot_id.c_str(),
      params.file_path.c_str(),
      params.file_ver.c_str(),
      params.install_dir.c_str(),
      calculated_xxhash.c_str());

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(conn_.GetHandle(), sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    LOG_ERROR("Failed to prepare is_file_valid query: %s",
              sqlite3_errmsg(conn_.GetHandle()));
    return false;
  }

  auto stmt_guard = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
      stmt, sqlite3_finalize);

  sqlite3_bind_text(stmt, 1, params.branch.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, params.package_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, params.build_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, params.depot_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, params.file_path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, params.file_ver.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 7, params.install_dir.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 8, calculated_xxhash.c_str(), -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    int count = sqlite3_column_int(stmt, 0);
    LOG_DEBUG("Database query result: count=%d", count);
    bool is_valid = count > 0;
    LOG_DEBUG("File validation result for %s: %s (skip download)",
              full_path.string().c_str(),
              is_valid ? "VALID" : "INVALID");
    return is_valid;
  } else if (rc != SQLITE_DONE) {
    LOG_ERROR("Failed to execute is_file_valid query: %s",
              sqlite3_errmsg(conn_.GetHandle()));
  }

  LOG_DEBUG("File validation result for %s: INVALID (database query failed)",
            full_path.string().c_str());
  return false;
}

}  // namespace Database
}  // namespace Quaton
