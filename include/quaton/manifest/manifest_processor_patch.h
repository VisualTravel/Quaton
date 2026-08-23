#ifndef QUATON_MANIFEST_PROCESSOR_PATCH_H_
#define QUATON_MANIFEST_PROCESSOR_PATCH_H_
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "quaton/http_client.h"

namespace Quaton {

// Compile-time constants
constexpr size_t kMaxPatchFileCount = 10000;    ///< Maximum patch file count
constexpr size_t kMaxObsoleteFileCount = 5000;  ///< Maximum obsolete file count

// ============================================================================
// PatchManifest - Patch manifest processing class (incremental update)
// ============================================================================

/**
 * @struct PatchFileMetadata
 * @brief Patch file information structure (corresponding to
 * SophonPatchAssetProperty)
 */
struct PatchFileMetadata {
  std::string target_path_;         ///< Target file path
  std::string target_md5_;          ///< Target file MD5
  int64_t target_size_;             ///< Target file size
  std::string source_version_tag_;  ///< Applicable source version tag

  // Patch detailed information
  std::string patch_identifier_;  ///< Patch file ID
  int64_t patch_total_size_;      ///< Patch file total size
  std::string patch_md5_;         ///< Patch file MD5
  int64_t patch_data_offset_;     ///< Patch data offset in file
  int64_t patch_data_length_;     ///< Patch data length
  std::string source_file_name_;  ///< Source file name
  int64_t source_file_size_;      ///< Source file size
  std::string source_file_md5_;   ///< Source file MD5

  bool is_new_file_;  ///< Whether it is a new file (source_file_name is empty)
  bool is_compressed_;  ///< Whether patch data is compressed (from
                        ///< manifest_mapping)

  /**
   * @brief Validate patch file metadata validity
   * @return Returns true if validation succeeds, otherwise false
   */
  bool is_valid() const noexcept;
};

/**
 * @struct ObsoleteFileMetadata
 * @brief Obsolete file information structure
 */
struct ObsoleteFileMetadata {
  std::string file_path_;  ///< File path
  int64_t file_size_;      ///< File size
  std::string file_md5_;   ///< File MD5

  /**
   * @brief Validate obsolete file metadata validity
   * @return Returns true if validation succeeds, otherwise false
   */
  bool is_valid() const noexcept;
};

/**
 * @class PatchManifestProcessor
 * @brief Patch manifest manager
 *
 * Responsible for downloading, parsing and managing Sophon Patch manifest,
 * providing patch file list and deletion file list
 */
class PatchManifestProcessor {
 public:
  /**
   * @brief Download and parse patch manifest from URL
   * @param http_client HTTP client instance
   * @param manifest_url Manifest file URL
   * @param compressed Whether manifest is compressed with Zstandard
   * @param expected_md5 Expected MD5 checksum
   * @param source_version Source version number (for filtering applicable
   * patches)
   * @param manifest_identifier Manifest identifier, used to save file name
   * @param output_dir Output directory, used to save original manifest file
   * (optional)
   * @return shared_ptr to PatchManifestProcessor instance
   * @throws std::invalid_argument If parameters are invalid
   * @throws std::runtime_error If download or parsing fails
   */
  static std::shared_ptr<PatchManifestProcessor> download_and_parse_manifest(
      const std::shared_ptr<HttpClient>& http_client,
      const std::string& manifest_url,
      bool compressed,
      const std::string& expected_md5,
      const std::string& source_version,
      const std::string& manifest_identifier = "",
      const std::string& output_dir = "");

  /**
   * @brief Parse patch manifest from local file
   * @param manifest_path Local manifest file path
   * @param compressed Whether manifest is compressed with Zstandard
   * @param source_version Source version number (for filtering applicable
   * patches)
   * @return shared_ptr to PatchManifestProcessor instance
   * @throws std::invalid_argument If parameters are invalid
   * @throws std::runtime_error If file reading or parsing fails
   */
  static std::shared_ptr<PatchManifestProcessor> parse_manifest_from_file(
      const std::string& manifest_path,
      bool compressed,
      const std::string& source_version);

  /**
   * @brief Parse patch manifest from memory data
   * @param data Protobuf data
   * @param source_version Source version number (for filtering applicable
   * patches)
   * @return shared_ptr to PatchManifestProcessor instance
   * @throws std::invalid_argument If parameters are invalid
   * @throws std::runtime_error If parsing fails
   */
  static std::shared_ptr<PatchManifestProcessor> parse_manifest_from_data(
      const std::vector<uint8_t>& data, const std::string& source_version);

  // Getters
  size_t get_patch_file_count() const noexcept {
    return patch_file_list_.size();
  }
  size_t get_obsolete_file_count() const noexcept {
    return obsolete_file_list_.size();
  }
  const std::vector<ObsoleteFileMetadata>& get_obsolete_file_list()
      const noexcept {
    return obsolete_file_list_;
  }
  const std::string& get_source_version() const noexcept {
    return source_version_;
  }
  const std::vector<PatchFileMetadata>& get_patch_file_list() const noexcept {
    return patch_file_list_;
  }
  const std::unordered_map<std::string, std::pair<int64_t, std::string>>&
  get_patch_detail_map() const noexcept {
    return patch_detail_map_;
  }

  // Setters
  void set_source_version(const std::string& source_version) {
    source_version_ = source_version;
  }
  void add_obsolete_file(const ObsoleteFileMetadata& obsolete_file) {
    obsolete_file_list_.push_back(obsolete_file);
  }
  void add_obsolete_file(ObsoleteFileMetadata&& obsolete_file) {
    obsolete_file_list_.push_back(std::move(obsolete_file));
  }
  void add_patch_file(const PatchFileMetadata& patch_file) {
    patch_file_list_.push_back(patch_file);
  }
  void add_patch_file(PatchFileMetadata&& patch_file) {
    patch_file_list_.push_back(std::move(patch_file));
  }

  /**
   * @brief Set compression flag for all patch files
   * @param is_compressed Whether patches are compressed (from manifest_mapping)
   */
  void set_compression_flag(bool is_compressed) {
    for (auto& patch_file : patch_file_list_) {
      patch_file.is_compressed_ = is_compressed;
    }
  }

  /**
   * @brief Get list of all unique patch IDs (for download deduplication)
   * @return Unique patch ID list
   */
  std::vector<std::string> get_distinct_patch_identifiers() const;

  /**
   * @brief Get patch file size and MD5 by patch ID
   * @param patch_id Patch ID
   * @param out_size Output: patch file size
   * @param out_md5 Output: patch file MD5
   * @return Whether the patch was found
   */
  bool retrieve_patch_details(const std::string& patch_id,
                              int64_t& out_size,
                              std::string& out_md5) const;

  /**
   * @brief Get total patch size (sum of all patch file sizes, deduplicated)
   * @return Total patch size (bytes)
   */
  int64_t calculate_total_patch_size() const;

 public:
  /**
   * @brief Constructor (for internal use only)
   * @param source_version Source version number
   */
  explicit PatchManifestProcessor(const std::string& source_version)
      : source_version_(source_version) {}

 private:
  /**
   * @brief Private default constructor - use static factory methods to create
   * instances
   */
  PatchManifestProcessor() = default;

  std::vector<PatchFileMetadata> patch_file_list_;  ///< Patch file list
  std::vector<ObsoleteFileMetadata>
      obsolete_file_list_;      ///< Obsolete file list
  std::string source_version_;  ///< Source version number

  // For quick patch information query (patch ID -> <size, MD5>)
  std::unordered_map<std::string, std::pair<int64_t, std::string>>
      patch_detail_map_;
};

// Compile-time checks
static_assert(std::is_nothrow_move_constructible_v<PatchFileMetadata>,
              "PatchFileMetadata should be nothrow move constructible");
static_assert(std::is_nothrow_move_constructible_v<ObsoleteFileMetadata>,
              "ObsoleteFileMetadata should be nothrow move constructible");

}  // namespace Quaton

#endif  // QUATON_MANIFEST_PROCESSOR_PATCH_H_
