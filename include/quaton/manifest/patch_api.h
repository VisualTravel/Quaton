#ifndef QUATON_MANIFEST_PATCH_API_H_
#define QUATON_MANIFEST_PATCH_API_H_
#pragma once

#include <map>
#include <string>
#include <vector>

#include "quaton/manifest/types.h"

namespace Quaton {

/**
 * @brief Patch operation statistics for specific version differences
 */
class PatchOperationStatistics {
 public:
  PatchOperationStatistics() = default;
  PatchOperationStatistics(int64_t compressed_bytes,
                           int64_t uncompressed_bytes,
                           int file_quantity,
                           int chunk_quantity);

  // Move constructor and assignment
  PatchOperationStatistics(PatchOperationStatistics&& other) noexcept = default;
  PatchOperationStatistics& operator=(
      PatchOperationStatistics&& other) noexcept = default;

  // Accessors
  int64_t compressed_size_bytes() const { return compressed_size_bytes_; }
  int64_t uncompressed_size_bytes() const { return uncompressed_size_bytes_; }
  int file_count() const { return file_count_; }
  int chunk_count() const { return chunk_count_; }

  // Modifiers
  void set_compressed_size_bytes(int64_t size) {
    compressed_size_bytes_ = size;
  }
  void set_uncompressed_size_bytes(int64_t size) {
    uncompressed_size_bytes_ = size;
  }
  void set_file_count(int count) { file_count_ = count; }
  void set_chunk_count(int count) { chunk_count_ = count; }

  /// @brief Calculate total size difference
  /// @return Difference between uncompressed and compressed sizes
  int64_t get_total_size_difference() const;

  /// @brief Check if statistics represent empty patch
  /// @return Returns true if all counts and sizes are zero
  bool is_empty() const;

 private:
  int64_t compressed_size_bytes_ = 0;
  int64_t uncompressed_size_bytes_ = 0;
  int file_count_ = 0;
  int chunk_count_ = 0;
};

/**
 * @brief Patch manifest category identity with differential download support
 */
class PatchManifestCategoryIdentity {
 public:
  PatchManifestCategoryIdentity() = default;

  // Move constructor and assignment operator
  PatchManifestCategoryIdentity(
      PatchManifestCategoryIdentity&& other) noexcept = default;
  PatchManifestCategoryIdentity& operator=(
      PatchManifestCategoryIdentity&& other) noexcept = default;

  // Accessors
  const std::string& category_identifier() const {
    return category_identifier_;
  }
  const std::string& category_display_name() const {
    return category_display_name_;
  }
  const std::string& matching_pattern() const { return matching_pattern_; }
  const ManifestFileMetadata& manifest_metadata() const {
    return manifest_metadata_;
  }
  const ManifestUrlConfiguration& manifest_download_config() const {
    return manifest_download_config_;
  }
  const ManifestUrlConfiguration& differential_download_config() const {
    return differential_download_config_;
  }
  const std::map<std::string, PatchOperationStatistics>& version_statistics()
      const {
    return version_statistics_;
  }

  // Modifiers
  void set_category_identifier(std::string id) {
    category_identifier_ = std::move(id);
  }
  void set_category_display_name(std::string name) {
    category_display_name_ = std::move(name);
  }
  void set_matching_pattern(std::string pattern) {
    matching_pattern_ = std::move(pattern);
  }
  void set_manifest_metadata(ManifestFileMetadata metadata) {
    manifest_metadata_ = std::move(metadata);
  }
  void set_manifest_download_config(ManifestUrlConfiguration config) {
    manifest_download_config_ = std::move(config);
  }
  void set_differential_download_config(ManifestUrlConfiguration config) {
    differential_download_config_ = std::move(config);
  }
  void set_version_statistics(
      std::map<std::string, PatchOperationStatistics> stats) {
    version_statistics_ = std::move(stats);
  }

  /// @brief Check if category supports specific version
  /// @param version_tag Version tag to check
  /// @return Returns true if version has statistics data
  bool supports_version(const std::string& version_tag) const;

  /// @brief Get statistics for specific version
  /// @param version Version tag
  /// @return Pointer to statistics data or nullptr (if not found)
  const PatchOperationStatistics* get_statistics_for_version(
      const std::string& version) const;

 private:
  std::string category_identifier_;
  std::string category_display_name_;
  std::string matching_pattern_;
  ManifestFileMetadata manifest_metadata_;
  ManifestUrlConfiguration manifest_download_config_;
  ManifestUrlConfiguration differential_download_config_;
  std::map<std::string, PatchOperationStatistics> version_statistics_;
};

/**
 * @brief Data payload of patch build API response
 */
class PatchBuildApiPayload {
 public:
  PatchBuildApiPayload() = default;

  // Move constructor and assignment operator
  PatchBuildApiPayload(PatchBuildApiPayload&& other) noexcept = default;
  PatchBuildApiPayload& operator=(PatchBuildApiPayload&& other) noexcept =
      default;

  // Accessors
  const std::string& build_identifier() const { return build_identifier_; }
  const std::string& patch_identifier() const { return patch_identifier_; }
  const std::string& target_version_tag() const { return target_version_tag_; }
  const std::vector<PatchManifestCategoryIdentity>& patch_manifest_categories()
      const {
    return patch_manifest_categories_;
  }

  // Modifiers
  void set_build_identifier(std::string id) {
    build_identifier_ = std::move(id);
  }
  void set_patch_identifier(std::string id) {
    patch_identifier_ = std::move(id);
  }
  void set_target_version_tag(std::string tag) {
    target_version_tag_ = std::move(tag);
  }
  void set_patch_manifest_categories(
      std::vector<PatchManifestCategoryIdentity> categories) {
    patch_manifest_categories_ = std::move(categories);
  }

  /// @brief Check if payload contains patch manifests
  /// @return Returns true if patch_manifest_categories is not empty
  bool has_patch_manifests() const {
    return !patch_manifest_categories_.empty();
  }

  /// @brief Calculate total patch size across all categories
  /// @return Total compressed size in bytes
  int64_t calculate_total_patch_size() const;

 private:
  std::string build_identifier_;
  std::string patch_identifier_;
  std::string target_version_tag_;
  std::vector<PatchManifestCategoryIdentity> patch_manifest_categories_;
};

/**
 * @brief Complete response structure for patch build API call
 */
class PatchBuildApiResponse {
 public:
  PatchBuildApiResponse() = default;
  PatchBuildApiResponse(int status_code,
                        std::string status_message,
                        PatchBuildApiPayload response_data);

  // Move constructor and assignment operator
  PatchBuildApiResponse(PatchBuildApiResponse&& other) noexcept = default;
  PatchBuildApiResponse& operator=(PatchBuildApiResponse&& other) noexcept =
      default;

  // Accessors
  int status_code() const { return status_code_; }
  const std::string& status_message() const { return status_message_; }
  const PatchBuildApiPayload& response_payload() const {
    return response_payload_;
  }

  // Modifiers
  void set_status_code(int code) { status_code_ = code; }
  void set_status_message(std::string message) {
    status_message_ = std::move(message);
  }
  void set_response_payload(PatchBuildApiPayload payload) {
    response_payload_ = std::move(payload);
  }

  /// @brief Check if response is successful
  /// @return Returns true if status_code is 0
  bool is_successful() const { return status_code_ == 0; }

  /// @brief Generate debug string representation
  /// @return Debug string containing response details
  std::string to_debug_string() const;

 private:
  int status_code_ = -1;
  std::string status_message_;
  PatchBuildApiPayload response_payload_;
};

}  // namespace Quaton

#endif  // QUATON_MANIFEST_PATCH_API_H_
