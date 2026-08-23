#ifndef PATCH_H
#define PATCH_H

#pragma once

#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "quaton/downloader/speed_limiter.h"
#include "quaton/manifest/chunk_manifest_pair.h"

namespace Quaton {

// Forward declarations
class Resource;
class HttpClient;

// ============================================================================
// PatchInfo - Patch file information for new architecture
// ============================================================================

/**
 * @struct PatchInfo
 * @brief Information about a patch file
 *
 * Used by the new Service -> Manager -> Scheduler architecture
 */
struct PatchInfo {
  std::string patch_name;   ///< Patch file name
  int64_t patch_size = 0;   ///< Patch file size
  std::string patch_hash;   ///< Patch file hash (MD5)
  std::string source_file;  ///< Source file name (to be patched)
  std::string target_file;  ///< Target file name (output)
  std::string target_hash;  ///< Expected target hash after patching
  int64_t target_size = 0;  ///< Expected target size after patching
  std::string patch_url;    ///< Download URL for patch
};

// ============================================================================
// QuatonPatchBranch - Patch branch information
// ============================================================================

/**
 * @class QuatonPatchBranch
 * @brief Quaton patch branch
 *
 * Represents patch branch information, including patch name, version and
 * related file list
 */
class QuatonPatchBranch {
 public:
  std::string patch_name;                ///< Patch name
  std::string patch_version;             ///< Patch version
  std::string from_version;              ///< Starting version
  std::string to_version;                ///< Target version
  std::vector<std::string> patch_files;  ///< Patch file list

  /**
   * @brief Default constructor
   */
  QuatonPatchBranch() noexcept = default;

  /**
   * @brief Constructor
   * @param name Patch name
   * @param from_ver Starting version
   * @param to_ver Target version
   */
  explicit QuatonPatchBranch(std::string_view name,
                             std::string_view from_ver,
                             std::string_view to_ver)
      : patch_name(name), from_version(from_ver), to_version(to_ver) {}

  /**
   * @brief Move constructor
   */
  QuatonPatchBranch(QuatonPatchBranch&& other) noexcept = default;

  /**
   * @brief Move assignment operator
   */
  QuatonPatchBranch& operator=(QuatonPatchBranch&& other) noexcept = default;

  /**
   * @brief Destructor
   */
  ~QuatonPatchBranch() noexcept = default;
};

// ============================================================================
// QuatonPatch - Patch processing
// ============================================================================

/**
 * @class QuatonPatch
 * @brief Quaton patch processing
 *
 * Provides static methods for patch resource enumeration and manifest info pair
 * creation
 */
class QuatonPatch {
 public:
  /**
   * @brief Enumerate patch resources
   * @param http_client HTTP client instance
   * @param info_pair Manifest info pair
   * @param download_speed_limiter_state Download speed limiter state (optional)
   * @param download_over_url Download URL (optional)
   * @param version_tag_update_from Update starting version tag (optional)
   * @param output_dir Output directory to save original and decrypted manifest
   * files (optional)
   * @return Resource list
   * @throws std::invalid_argument If http_client or info_pair is invalid
   */
  static std::vector<std::shared_ptr<Resource>> enumerate_patch_async(
      const std::shared_ptr<HttpClient>& http_client,
      const ChunkManifestPair& info_pair,
      std::shared_ptr<SpeedLimiter::State> download_speed_limiter_state =
          nullptr,
      std::string_view download_over_url = "",
      std::string_view version_tag_update_from = "",
      std::string_view output_dir = "");

  /**
   * @brief Create manifest info pair
   * @param url URL
   * @param version_update_from Update starting version
   * @param matching_field Matching field, defaults to "game"
   * @return Future object of manifest info pair
   * @throws std::invalid_argument If url is empty
   */
  static std::future<ChunkManifestPair> create_quaton_chunk_manifest_info_pair(
      std::string_view url,
      std::string_view version_update_from,
      std::string_view matching_field = "game");

 private:
  /**
   * @brief Private constructor to prevent instantiation
   */
  QuatonPatch() = delete;
};

}  // namespace Quaton

#endif  // PATCH_H
