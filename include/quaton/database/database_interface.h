#ifndef QUATON_DATABASE_DATABASE_INTERFACE_H
#define QUATON_DATABASE_DATABASE_INTERFACE_H
#pragma once

#include <string>
#include <vector>

#include "quaton/database/database_manager.h"

namespace Quaton {

/**
 * @brief Update chunk configuration in database
 * @param config Configuration record to update
 * @return Whether update was successful
 */
bool UpdateChunkConfig(const ConfigRecord& config);

/**
 * @brief Query file records from database
 * @param package_id Package identifier
 * @param file_id File identifier
 * @return Vector of file records
 */
std::vector<FileRecord> QueryFileRecords(const std::string& package_id,
                                         const std::string& file_id = "");

/**
 * @brief Query config records from database
 * @param package_id Package identifier
 * @return Vector of config records
 */
std::vector<ConfigRecord> QueryConfigRecords(
    const std::string& package_id = "");

/**
 * @brief Get latest configuration for a package
 * @param package_id Package identifier
 * @return Latest config record
 */
ConfigRecord GetLatestConfig(const std::string& package_id);

/**
 * @brief Query manifest records from database
 * @param package_id Package identifier
 * @param build_id Build identifier
 * @return Vector of manifest records
 */
std::vector<ManifestRecord> QueryManifestRecords(
    const std::string& package_id, const std::string& build_id = "");

}  // namespace Quaton

#endif  // QUATON_DATABASE_DATABASE_INTERFACE_H
