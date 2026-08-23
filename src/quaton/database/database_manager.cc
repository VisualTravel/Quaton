#include "quaton/database/database_manager.h"

#include <filesystem>
#include <stdexcept>

#include "quaton/configuration/database_config.h"
#include "quaton/logger.h"
#include "quaton/utils/path_helper.h"

namespace fs = std::filesystem;

namespace Quaton {

DatabaseManager& DatabaseManager::Instance() {
  static DatabaseManager instance(PathHelper::GetDatabaseDir());
  return instance;
}

DatabaseManager::DatabaseManager(const std::string& db_dir) : db_dir_(db_dir) {
  if (db_dir_.empty()) {
    throw std::invalid_argument("Database directory cannot be empty");
  }

  fs::path db_path(db_dir_);

  DatabaseConfig chunk_config = DatabaseConfig::with_path(db_dir_, "chunk.db");
  DatabaseConfig config_db_config =
      DatabaseConfig::with_path(db_dir_, "chunk_config.db");
  DatabaseConfig manifest_config =
      DatabaseConfig::with_path(db_dir_, "chunk_manifest.db");

  chunk_db_ =
      std::make_unique<Database::ChunkDatabase>(chunk_config.get_full_path());
  config_db_ = std::make_unique<Database::ConfigDatabase>(
      config_db_config.get_full_path());
  manifest_db_ = std::make_unique<Database::ManifestDatabase>(
      manifest_config.get_full_path());

  if (!Initialize()) {
    throw std::runtime_error("Failed to initialize database manager");
  }
}

bool DatabaseManager::Initialize() {
  try {
    if (!fs::exists(db_dir_)) {
      fs::create_directories(db_dir_);
      LOG_INFO("Created database directory: %s", db_dir_.c_str());
    }

    if (!chunk_db_->Initialize()) {
      LOG_ERROR("Failed to initialize chunk database");
      return false;
    }

    if (!config_db_->Initialize()) {
      LOG_ERROR("Failed to initialize config database");
      return false;
    }

    if (!manifest_db_->Initialize()) {
      LOG_ERROR("Failed to initialize manifest database");
      return false;
    }

    LOG_INFO("Database manager initialized successfully");
    return true;
  } catch (const std::exception& e) {
    LOG_ERROR("Exception during database initialization: %s", e.what());
    return false;
  }
}

// ==================== Chunk.db operations (delegated) ====================

bool DatabaseManager::InsertFileRecord(const FileRecord& record) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  return chunk_db_->InsertRecord(record);
}

bool DatabaseManager::InsertFileRecords(
    const std::vector<FileRecord>& records) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  return chunk_db_->InsertRecords(records);
}

std::vector<FileRecord> DatabaseManager::QueryFileRecords(
    const std::string& package_id, const std::string& file_id) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  return chunk_db_->QueryRecords(package_id, file_id);
}

bool DatabaseManager::UpdateFileStatus(const std::string& package_id,
                                       const std::string& branch,
                                       const std::string& file_id,
                                       int status) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  return chunk_db_->UpdateFileStatus(package_id, branch, file_id, status);
}

bool DatabaseManager::IsFileValid(const FileValidationParams& params) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  return chunk_db_->IsFileValid(params);
}

bool DatabaseManager::DeleteOldFileRecords(const std::string& package_id,
                                           int keep_versions) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  return chunk_db_->DeleteOldRecords(package_id, keep_versions);
}

// ==================== Config.db operations (delegated) ====================

bool DatabaseManager::UpsertConfigRecord(const ConfigRecord& record) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  return config_db_->UpsertRecord(record);
}

std::vector<ConfigRecord> DatabaseManager::QueryConfigRecords(
    const std::string& package_id) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  return config_db_->QueryRecords(package_id);
}

ConfigRecord DatabaseManager::GetLatestConfig(const std::string& package_id) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  return config_db_->GetLatestRecord(package_id);
}

bool DatabaseManager::DeleteOldConfigRecords(const std::string& package_id,
                                             int keep_versions) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  return config_db_->DeleteOldRecords(package_id, keep_versions);
}

bool DatabaseManager::UpdateChunkConfig(const ConfigRecord& record) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  return UpsertConfigRecord(record);
}

// ==================== Manifest.db operations (delegated) ====================

bool DatabaseManager::InsertManifestRecord(const ManifestRecord& record) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  return manifest_db_->InsertRecord(record);
}

bool DatabaseManager::InsertManifestRecords(
    const std::vector<ManifestRecord>& records) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  return manifest_db_->InsertRecords(records);
}

std::vector<ManifestRecord> DatabaseManager::QueryManifestRecords(
    const std::string& package_id, const std::string& build_id) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  return manifest_db_->QueryRecords(package_id, build_id);
}

bool DatabaseManager::DeleteOldManifestRecords(const std::string& package_id,
                                               int keep_versions) {
  std::lock_guard<std::recursive_mutex> lock(db_mutex_);
  return manifest_db_->DeleteOldRecords(package_id, keep_versions);
}

}  // namespace Quaton
