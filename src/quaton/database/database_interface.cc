#include "quaton/database/database_interface.h"

#include "quaton/logger.h"

namespace Quaton {

bool UpdateChunkConfig(const ConfigRecord& config) {
  try {
    return DatabaseManager::Instance().UpdateChunkConfig(config);
  } catch (const std::exception& e) {
    LOG_ERROR("Failed to update chunk config: %s", e.what());
    return false;
  }
}

std::vector<FileRecord> QueryFileRecords(const std::string& package_id,
                                         const std::string& file_id) {
  try {
    return DatabaseManager::Instance().QueryFileRecords(package_id, file_id);
  } catch (const std::exception& e) {
    LOG_ERROR("Failed to query file records: %s", e.what());
    return {};
  }
}

std::vector<ConfigRecord> QueryConfigRecords(const std::string& package_id) {
  try {
    return DatabaseManager::Instance().QueryConfigRecords(package_id);
  } catch (const std::exception& e) {
    LOG_ERROR("Failed to query config records: %s", e.what());
    return {};
  }
}

ConfigRecord GetLatestConfig(const std::string& package_id) {
  try {
    return DatabaseManager::Instance().GetLatestConfig(package_id);
  } catch (const std::exception& e) {
    LOG_ERROR("Failed to get latest config: %s", e.what());
    return ConfigRecord{};
  }
}

std::vector<ManifestRecord> QueryManifestRecords(const std::string& package_id,
                                                 const std::string& build_id) {
  try {
    return DatabaseManager::Instance().QueryManifestRecords(package_id,
                                                            build_id);
  } catch (const std::exception& e) {
    LOG_ERROR("Failed to query manifest records: %s", e.what());
    return {};
  }
}

}  // namespace Quaton
