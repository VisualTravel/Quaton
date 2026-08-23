#include "quaton/database/manifest_db.h"

#include <stdexcept>

#include "quaton/database/transaction.h"
#include "quaton/logger.h"

namespace Quaton {
namespace Database {

ManifestDatabase::ManifestDatabase(const std::string& db_path)
    : conn_(db_path) {
}

bool ManifestDatabase::Initialize() {
  if (!conn_.Open()) {
    return false;
  }

  return CreateTables();
}

bool ManifestDatabase::CreateTables() {
  std::string create_table_sql = R"(
        CREATE TABLE IF NOT EXISTS manifests (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            package_id TEXT NOT NULL,
            build_id TEXT NOT NULL,
            depot_id TEXT NOT NULL,
            version TEXT NOT NULL,
            depot_manifest_id TEXT NOT NULL,
            depot_manifest_checksum TEXT NOT NULL,
            depot_manifest_compressed_size INTEGER NOT NULL,
            depot_manifest_uncompressed_size INTEGER NOT NULL,
            depot_manifest_url_prefix TEXT NOT NULL,
            depot_manifest_encryption INTEGER NOT NULL,
            depot_manifest_password TEXT,
            depot_manifest_compression INTEGER NOT NULL,
            depot_manifest_md5 TEXT NOT NULL,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(package_id, build_id, depot_id)
        );
        CREATE INDEX IF NOT EXISTS idx_manifests_package ON manifests(package_id);
        CREATE INDEX IF NOT EXISTS idx_manifests_build ON manifests(build_id);
    )";

  std::string error_msg;
  if (!conn_.Execute(create_table_sql, error_msg)) {
    LOG_ERROR("Failed to create manifest table: %s", error_msg.c_str());
    return false;
  }

  return true;
}

bool ManifestDatabase::InsertRecord(const ManifestRecord& record) {
  if (record.package_id.empty() || record.build_id.empty()) {
    throw std::invalid_argument("Package ID and build ID cannot be empty");
  }

  const char* sql = R"(
        INSERT OR REPLACE INTO manifests
        (package_id, build_id, depot_id, version, depot_manifest_id,
         depot_manifest_checksum, depot_manifest_compressed_size,
         depot_manifest_uncompressed_size, depot_manifest_url_prefix,
         depot_manifest_encryption, depot_manifest_password,
         depot_manifest_compression, depot_manifest_md5)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(conn_.GetHandle(), sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    LOG_ERROR("Failed to prepare manifest insert: %s",
              sqlite3_errmsg(conn_.GetHandle()));
    return false;
  }

  auto stmt_guard = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
      stmt, sqlite3_finalize);

  sqlite3_bind_text(stmt, 1, record.package_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, record.build_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, record.depot_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, record.version.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(
      stmt, 5, record.depot_manifest_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(
      stmt, 6, record.depot_manifest_checksum.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 7, record.depot_manifest_compressed_size);
  sqlite3_bind_int64(stmt, 8, record.depot_manifest_uncompressed_size);
  sqlite3_bind_text(
      stmt, 9, record.depot_manifest_url_prefix.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 10, record.depot_manifest_encryption);
  sqlite3_bind_text(
      stmt, 11, record.depot_manifest_password.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 12, record.depot_manifest_compression);
  sqlite3_bind_text(
      stmt, 13, record.depot_manifest_md5.c_str(), -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    LOG_ERROR("Failed to insert manifest record: %s",
              sqlite3_errmsg(conn_.GetHandle()));
    return false;
  }

  return true;
}

bool ManifestDatabase::InsertRecords(
    const std::vector<ManifestRecord>& records) {
  if (records.empty()) {
    LOG_DEBUG("No manifest records to insert");
    return true;
  }

  return ExecuteInTransaction(conn_.GetHandle(), [&]() {
    for (const auto& record : records) {
      try {
        if (!InsertRecord(record)) {
          LOG_ERROR("Failed to insert manifest record for build: %s",
                    record.build_id.c_str());
          return false;
        }
      } catch (const std::exception& e) {
        LOG_ERROR("Exception during manifest bulk insert: %s", e.what());
        return false;
      }
    }
    return true;
  });
}

std::vector<ManifestRecord> ManifestDatabase::QueryRecords(
    const std::string& package_id, const std::string& build_id) {
  if (package_id.empty()) {
    throw std::invalid_argument("Package ID cannot be empty");
  }

  std::vector<ManifestRecord> records;

  std::string sql =
      "SELECT id, package_id, build_id, depot_id, version, depot_manifest_id, "
      "depot_manifest_checksum, depot_manifest_compressed_size, "
      "depot_manifest_uncompressed_size, depot_manifest_url_prefix, "
      "depot_manifest_encryption, depot_manifest_password, "
      "depot_manifest_compression, depot_manifest_md5, created_at FROM "
      "manifests WHERE package_id = ?";
  if (!build_id.empty()) {
    sql += " AND build_id = ?";
  }
  sql += " ORDER BY created_at DESC;";

  sqlite3_stmt* stmt = nullptr;
  int rc =
      sqlite3_prepare_v2(conn_.GetHandle(), sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    LOG_ERROR("Failed to prepare manifest query: %s",
              sqlite3_errmsg(conn_.GetHandle()));
    return records;
  }

  auto stmt_guard = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(
      stmt, sqlite3_finalize);

  sqlite3_bind_text(stmt, 1, package_id.c_str(), -1, SQLITE_TRANSIENT);
  if (!build_id.empty()) {
    sqlite3_bind_text(stmt, 2, build_id.c_str(), -1, SQLITE_TRANSIENT);
  }

  records.reserve(50);

  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    ManifestRecord record;
    record.id = sqlite3_column_int(stmt, 0);
    record.package_id =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    record.build_id =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    record.depot_id =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    record.version =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    record.depot_manifest_id =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    record.depot_manifest_checksum =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    record.depot_manifest_compressed_size = sqlite3_column_int64(stmt, 7);
    record.depot_manifest_uncompressed_size = sqlite3_column_int64(stmt, 8);
    record.depot_manifest_url_prefix =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
    record.depot_manifest_encryption = sqlite3_column_int(stmt, 10);

    const char* password =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
    if (password) {
      record.depot_manifest_password = password;
    }

    record.depot_manifest_compression = sqlite3_column_int(stmt, 12);
    record.depot_manifest_md5 =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
    records.push_back(std::move(record));
  }

  if (rc != SQLITE_DONE) {
    LOG_ERROR("Manifest query execution failed: %s",
              sqlite3_errmsg(conn_.GetHandle()));
    records.clear();
  }

  return records;
}

bool ManifestDatabase::DeleteOldRecords(const std::string& package_id,
                                        int keep_versions) {
  if (package_id.empty()) {
    throw std::invalid_argument("Package ID cannot be empty");
  }
  if (keep_versions < 1) {
    throw std::invalid_argument("Keep versions must be at least 1");
  }

  const char* sql = R"(
        DELETE FROM manifests
        WHERE package_id = ?
        AND version NOT IN (
            SELECT DISTINCT version FROM manifests
            WHERE package_id = ?
            ORDER BY created_at DESC
            LIMIT ?
        );
    )";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(conn_.GetHandle(), sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    LOG_ERROR("Failed to prepare manifest delete: %s",
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
    LOG_ERROR("Failed to delete old manifest records: %s",
              sqlite3_errmsg(conn_.GetHandle()));
    return false;
  }

  if (deleted > 0) {
    LOG_INFO("Deleted %d old manifest records for package %s",
             deleted,
             package_id.c_str());
  }

  return true;
}

}  // namespace Database
}  // namespace Quaton
