#include "quaton/quaton.h"

#include <curl/curl.h>
#include <zstd.h>

#include <sstream>

#include "quaton/database/database_interface.h"
#include "quaton/logger.h"
#include "quaton/utils/hpatch.h"

namespace Quaton {

// Initialize Logger (static initialization)
static bool logger_initialized = []() { return Logger::initialize(); }();

std::string get_build_info() {
  std::ostringstream oss;
  oss << "Quaton " << kVersion << "\n";
#if defined(_WIN32)
  oss << "  platform  : Windows x64\n";
#elif defined(__linux__)
  oss << "  platform  : Linux\n";
#else
  oss << "  platform  : Unknown\n";
#endif
#if defined(_MSC_VER)
  oss << "  compiler  : MSVC " << (_MSC_VER / 100) << '.' << (_MSC_VER % 100)
      << "\n";
#elif defined(__clang__)
  oss << "  compiler  : Clang " << __clang_major__ << "\n";
#elif defined(__GNUC__)
  oss << "  compiler  : GCC " << __GNUC__ << "\n";
#endif
  oss << "  HDiffPatch: " << HPatchUtils::get_hpatchz_version() << "\n"
      << "  zstd      : " << ZSTD_versionString() << "\n"
      << "  libcurl   : " << curl_version() << "\n";
  return oss.str();
}

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
