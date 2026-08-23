#include "quaton/quaton.h"

#include "quaton/database/database_interface.h"
#include "quaton/logger.h"

namespace Quaton {

// Initialize Logger (static initialization)
static bool logger_initialized = []() { return Logger::initialize(); }();

bool update_chunk_config(const Quaton::ConfigRecord& config) {
  return UpdateChunkConfig(config);
}

std::vector<Quaton::FileRecord> query_file_records(
    const std::string& package_id, const std::string& file_id) {
  return QueryFileRecords(package_id, file_id);
}

std::vector<Quaton::ConfigRecord> query_config_records(
    const std::string& package_id) {
  return QueryConfigRecords(package_id);
}

Quaton::ConfigRecord get_latest_config(const std::string& package_id) {
  return GetLatestConfig(package_id);
}

std::vector<Quaton::ManifestRecord> query_manifest_records(
    const std::string& package_id, const std::string& build_id) {
  return QueryManifestRecords(package_id, build_id);
}

}  // namespace Quaton
