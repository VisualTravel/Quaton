#include "quaton/database/config_db.h"

#include <stdexcept>

#include "quaton/logger.h"

namespace Quaton {
namespace Database {

ConfigDatabase::ConfigDatabase(const std::string& db_path) : conn_(db_path) {
}

bool ConfigDatabase::Initialize() {
  if (!conn_.Open()) {
    return false;
  }

  return CreateTables();
}

bool ConfigDatabase::CreateTables() {
  std::string create_table_sql = R"(
        CREATE TABLE IF NOT EXISTS config (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            local_version TEXT NOT NULL,
            server_version TEXT NOT NULL,
            local_build_id TEXT NOT NULL,
            server_build_id TEXT NOT NULL,
            branch TEXT NOT NULL,
            package_id TEXT NOT NULL,
            depot_id TEXT NOT NULL,
            matching_field TEXT NOT NULL,
            install_dir TEXT NOT NULL,
            updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(branch, package_id)
        );
        CREATE INDEX IF NOT EXISTS idx_config_package ON config(package_id);
    )";

  std::string error_msg;
  if (!conn_.Execute(create_table_sql, error_msg)) {
    LOG_ERROR("Failed to create config table: %s", error_msg.c_str());
    return false;
  }

  return true;
}

bool ConfigDatabase::UpsertRecord(const ConfigRecord& record) {
  if (record.package_id.empty()) {
    throw std::invalid_argument("Package ID cannot be empty");
  }

  const char* sql = R"(
        INSERT OR REPLACE INTO config
        (local_version, server_version, local_build_id, server_build_id,
         branch, package_id, depot_id, matching_field, install_dir, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP);
    )";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(conn_.GetHandle(), sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    LOG_ERROR("Failed to prepare config insert: %s",
              sqlite3_errmsg(conn_.GetHandle()));
    return false;
  }

  auto stmt_guard = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
      stmt, sqlite3_finalize);

  sqlite3_bind_text(
      stmt, 1, record.local_version.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(
      stmt, 2, record.server_version.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(
      stmt, 3, record.local_build_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(
      stmt, 4, record.server_build_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, record.branch.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, record.package_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 7, record.depot_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(
      stmt, 8, record.matching_field.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 9, record.install_dir.c_str(), -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    LOG_ERROR("Failed to insert config record: %s",
              sqlite3_errmsg(conn_.GetHandle()));
    return false;
  }

  return true;
}

std::vector<ConfigRecord> ConfigDatabase::QueryRecords(
    const std::string& package_id) {
  std::vector<ConfigRecord> records;

  std::string sql =
      "SELECT id, local_version, server_version, local_build_id, "
      "server_build_id, branch, package_id, depot_id, matching_field, "
      "install_dir, updated_at FROM config";
  if (!package_id.empty()) {
    sql += " WHERE package_id = ?";
  }
  sql += " ORDER BY updated_at DESC;";

  sqlite3_stmt* stmt = nullptr;
  int rc =
      sqlite3_prepare_v2(conn_.GetHandle(), sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    LOG_ERROR("Failed to prepare config query: %s",
              sqlite3_errmsg(conn_.GetHandle()));
    return records;
  }

  auto stmt_guard = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
      stmt, sqlite3_finalize);

  if (!package_id.empty()) {
    sqlite3_bind_text(stmt, 1, package_id.c_str(), -1, SQLITE_TRANSIENT);
  }

  records.reserve(10);

  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    ConfigRecord record;
    record.id = sqlite3_column_int(stmt, 0);
    record.local_version =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    record.server_version =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    record.local_build_id =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    record.server_build_id =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    record.branch = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    record.package_id =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    record.depot_id =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
    record.matching_field =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
    record.install_dir =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
    records.push_back(std::move(record));
  }

  if (rc != SQLITE_DONE) {
    LOG_ERROR("Config query execution failed: %s",
              sqlite3_errmsg(conn_.GetHandle()));
    records.clear();
  }

  return records;
}

ConfigRecord ConfigDatabase::GetLatestRecord(const std::string& package_id) {
  if (package_id.empty()) {
    throw std::invalid_argument("Package ID cannot be empty");
  }

  auto records = QueryRecords(package_id);
  if (!records.empty()) {
    return records[0];
  }
  return ConfigRecord();
}

bool ConfigDatabase::DeleteOldRecords(const std::string& package_id,
                                      int keep_versions) {
  if (package_id.empty()) {
    throw std::invalid_argument("Package ID cannot be empty");
  }
  if (keep_versions < 1) {
    throw std::invalid_argument("Keep versions must be at least 1");
  }

  const char* sql = R"(
        DELETE FROM config
        WHERE package_id = ?
        AND id NOT IN (
            SELECT id FROM config
            WHERE package_id = ?
            ORDER BY updated_at DESC
            LIMIT ?
        );
    )";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(conn_.GetHandle(), sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    LOG_ERROR("Failed to prepare config delete: %s",
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
    LOG_ERROR("Failed to delete old config records: %s",
              sqlite3_errmsg(conn_.GetHandle()));
    return false;
  }

  if (deleted > 0) {
    LOG_INFO("Deleted %d old config records for package %s",
             deleted,
             package_id.c_str());
  }

  return true;
}

}  // namespace Database
}  // namespace Quaton
