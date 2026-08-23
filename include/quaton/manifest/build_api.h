#ifndef QUATON_MANIFEST_BUILD_API_H_
#define QUATON_MANIFEST_BUILD_API_H_
#pragma once

#include <string>
#include <vector>

#include "quaton/manifest/types.h"

namespace Quaton {

/**
 * @brief Manifest category identity information with download configuration
 */
class ManifestCategoryIdentity {
 public:
  ManifestCategoryIdentity() = default;

  // Move constructor and assignment operator
  ManifestCategoryIdentity(ManifestCategoryIdentity&& other) noexcept = default;
  ManifestCategoryIdentity& operator=(
      ManifestCategoryIdentity&& other) noexcept = default;

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
  const ManifestChunkStatistics& chunk_statistics() const {
    return chunk_statistics_;
  }
  const ManifestUrlConfiguration& chunk_download_config() const {
    return chunk_download_config_;
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
  void set_chunk_statistics(ManifestChunkStatistics stats) {
    chunk_statistics_ = std::move(stats);
  }
  void set_chunk_download_config(ManifestUrlConfiguration config) {
    chunk_download_config_ = std::move(config);
  }

  /// @brief Check if category identity is complete
  /// @return Returns true if all required fields are set and valid
  bool is_complete() const;

  /// @brief Get manifest download URL
  /// @return Complete manifest download URL
  std::string get_manifest_download_url() const;

 private:
  std::string category_identifier_;
  std::string category_display_name_;
  std::string matching_pattern_;
  ManifestFileMetadata manifest_metadata_;
  ManifestUrlConfiguration manifest_download_config_;
  ManifestChunkStatistics chunk_statistics_;
  ManifestUrlConfiguration chunk_download_config_;
};

/**
 * @brief Build API response payload data
 */
class BuildApiPayload {
 public:
  BuildApiPayload() = default;

  // Move constructor and assignment operator
  BuildApiPayload(BuildApiPayload&& other) noexcept = default;
  BuildApiPayload& operator=(BuildApiPayload&& other) noexcept = default;

  // Accessors
  const std::string& build_identifier() const { return build_identifier_; }
  const std::string& version_tag() const { return version_tag_; }
  const std::vector<ManifestCategoryIdentity>& manifest_categories() const {
    return manifest_categories_;
  }

  // Modifiers
  void set_build_identifier(std::string id) {
    build_identifier_ = std::move(id);
  }
  void set_version_tag(std::string tag) { version_tag_ = std::move(tag); }
  void set_manifest_categories(
      std::vector<ManifestCategoryIdentity> categories) {
    manifest_categories_ = std::move(categories);
  }

  /// @brief Check if payload contains manifests
  /// @return Returns true if manifest_categories is not empty
  bool has_manifests() const { return !manifest_categories_.empty(); }

  /// @brief Find manifest by matching pattern
  /// @param pattern Pattern to match
  /// @return Pointer to matching category or nullptr
  const ManifestCategoryIdentity* find_manifest_by_pattern(
      const std::string& pattern) const;

 private:
  std::string build_identifier_;
  std::string version_tag_;
  std::vector<ManifestCategoryIdentity> manifest_categories_;
};

/**
 * @brief Complete response structure for build API call
 */
class BuildApiResponse {
 public:
  BuildApiResponse() = default;
  BuildApiResponse(int status_code,
                   std::string status_message,
                   BuildApiPayload response_data);

  // Move constructor and assignment operator
  BuildApiResponse(BuildApiResponse&& other) noexcept = default;
  BuildApiResponse& operator=(BuildApiResponse&& other) noexcept = default;

  // Accessors
  int status_code() const { return status_code_; }
  const std::string& status_message() const { return status_message_; }
  const BuildApiPayload& response_payload() const { return response_payload_; }

  // Modifiers
  void set_status_code(int code) { status_code_ = code; }
  void set_status_message(std::string message) {
    status_message_ = std::move(message);
  }
  void set_response_payload(BuildApiPayload payload) {
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
  BuildApiPayload response_payload_;
};

}  // namespace Quaton

#endif  // QUATON_MANIFEST_BUILD_API_H_
