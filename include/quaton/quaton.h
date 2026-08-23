#pragma once

#include "quaton/chunk/chunk_download_service.h"
#include "quaton/configuration/configuration.h"
#include "quaton/database/database_manager.h"
#include "quaton/downloader/speed_limiter.h"
#include "quaton/http_client.h"
#include "quaton/manifest/chunk_manifest_pair.h"
#include "quaton/patch/patch_download_service.h"
#include "quaton/progress_callback.h"
#include "quaton/quaton_global.h"
#include "quaton/resource.h"
#include "quaton/url.h"

namespace Quaton {

/**
 * @brief Quaton library version information
 */
constexpr const char* kVersion = "1.0.0";

/**
 * @struct DownloadOptions
 * @brief Download options structure
 */
struct DownloadOptions {
  BranchType branch = BranchType::kMain;  ///< Branch type
  /// Optional self-hosted server base URL (e.g. "http://example.com:10001").
  /// When set, getBranches/getBuild/getPatchBuild resolve against it.
  std::string api_base_url;
  int threads = 0;  ///< Number of threads, 0 means use CPU core count
  int max_http_handles = 128;                    ///< Maximum HTTP handle count
  bool silent = false;                           ///< Whether silent mode
  ProgressCallback progress_callback = nullptr;  ///< Progress callback function
  bool install_enable = true;     ///< Whether to install update after download
                                  ///< verification, default true
  int buffer_size = 32;           ///< Buffer size, default 32
  int buffer_queue_size = 10240;  ///< Buffer queue size, default 10240
  int retry_on_download_failure =
      5;  ///< Retry count on download failure, default 5
  int request_timeout_wait_time =
      5;  ///< Request timeout wait time (seconds), default 5
  int max_concurrent_tasks =
      0;  ///< Maximum concurrent tasks, 0 means use maximum thread count
  int max_validation_threads =
      0;  ///< Maximum validation threads, 0 means use CPU core count
  bool multi_thread_io_read_enabled =
      true;  ///< Enable multi-threaded I/O reading, default true
  bool multi_thread_io_write_enabled =
      true;  ///< Enable multi-threaded I/O writing, default true
};

// ==================== Database related interfaces ====================
// Note: Database functions are now implemented in database_interface.h/cc

/**
 * @brief Update chunk_config configuration
 * @param config Configuration record
 * @return Whether update succeeded
 */
QUATON_API bool update_chunk_config(const Quaton::ConfigRecord& config);

/**
 * @brief Query file records for specified package
 * @param package_id Package ID
 * @param file_id File ID (optional, empty means query all files)
 * @return File record list
 */
QUATON_API std::vector<Quaton::FileRecord> query_file_records(
    const std::string& package_id, const std::string& file_id = "");

/**
 * @brief Query configuration records for specified package
 * @param package_id Package ID (optional, empty means query all)
 * @return Configuration record list
 */
QUATON_API std::vector<Quaton::ConfigRecord> query_config_records(
    const std::string& package_id = "");

/**
 * @brief Get latest configuration for a package
 * @param package_id Package ID
 * @return Latest configuration record
 */
QUATON_API Quaton::ConfigRecord get_latest_config(
    const std::string& package_id);

/**
 * @brief Query manifest records for specified package
 * @param package_id Package ID
 * @param build_id Build ID (optional, empty means query all builds)
 * @return Manifest record list
 */
QUATON_API std::vector<Quaton::ManifestRecord> query_manifest_records(
    const std::string& package_id, const std::string& build_id = "");

}  // namespace Quaton
