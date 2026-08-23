#ifndef QUATON_CHUNK_MANIFEST_PAIR_H_
#define QUATON_CHUNK_MANIFEST_PAIR_H_
#pragma once

#include <memory>
#include <string>

#include "quaton/data_chunk.h"
#include "quaton/manifest/manifest_metadata.h"

namespace Quaton {

// ============================================================================
// ChunkManifestPair - Data chunk and manifest information pair class
// ============================================================================

/**
 * @class ChunkManifestPair
 * @brief Quaton data chunk and manifest information pair class
 *
 * Combines data chunk information and manifest information to provide strong
 * exception safety guarantee
 */
class ChunkManifestPair {
 public:
  /**
   * @brief Default constructor
   */
  ChunkManifestPair() noexcept = default;

  /**
   * @brief Constructor
   * @param chunk_info Data chunk information
   * @param manifest_metadata Manifest information
   */
  ChunkManifestPair(Quaton::ChunkInfo chunk_info,
                    ManifestMetadata manifest_metadata);

  /**
   * @brief Move constructor
   * @param other Instance to move
   */
  ChunkManifestPair(ChunkManifestPair&& other) noexcept = default;

  /**
   * @brief Move assignment operator
   * @param other Instance to move
   * @return Reference to self
   */
  ChunkManifestPair& operator=(ChunkManifestPair&& other) noexcept;

  /**
   * @brief Copy constructor
   * @param other Instance to copy
   */
  ChunkManifestPair(const ChunkManifestPair& other) = default;

  /**
   * @brief Copy assignment operator
   * @param other Instance to copy
   * @return Reference to self
   */
  ChunkManifestPair& operator=(const ChunkManifestPair& other) = default;

  // Getters
  const ManifestMetadata& get_manifest_metadata() const noexcept {
    return manifest_metadata_;
  }
  ManifestMetadata& get_manifest_metadata() noexcept {
    return manifest_metadata_;
  }
  const Quaton::ChunkInfo& get_chunk_info() const noexcept {
    return chunk_info_;
  }
  Quaton::ChunkInfo& get_chunk_info() noexcept { return chunk_info_; }
  bool is_found() const noexcept { return found_; }
  int get_return_code() const noexcept { return return_code_; }
  const std::string& get_return_message() const noexcept {
    return return_message_;
  }
  const std::string& get_api_package_id() const noexcept {
    return api_package_id_;
  }
  const std::string& get_api_build_id() const noexcept { return api_build_id_; }
  const std::string& get_api_tag() const noexcept { return api_tag_; }
  const std::string& get_api_category_id() const noexcept {
    return api_category_id_;
  }
  const std::string& get_api_matching_field() const noexcept {
    return api_matching_field_;
  }

  // Setters
  void set_manifest_metadata(const ManifestMetadata& manifest_metadata) {
    manifest_metadata_ = manifest_metadata;
  }
  void set_manifest_metadata(ManifestMetadata&& manifest_metadata) noexcept {
    manifest_metadata_ = std::move(manifest_metadata);
  }
  void set_chunk_info(const Quaton::ChunkInfo& chunk_info) {
    chunk_info_ = chunk_info;
  }
  void set_chunk_info(Quaton::ChunkInfo&& chunk_info) noexcept {
    chunk_info_ = std::move(chunk_info);
  }
  void set_found(bool found) noexcept { found_ = found; }
  void set_return_code(int return_code) noexcept { return_code_ = return_code; }
  void set_return_message(const std::string& return_message) {
    return_message_ = return_message;
  }
  void set_return_message(std::string&& return_message) noexcept {
    return_message_ = std::move(return_message);
  }
  void set_api_package_id(const std::string& api_package_id) {
    api_package_id_ = api_package_id;
  }
  void set_api_package_id(std::string&& api_package_id) noexcept {
    api_package_id_ = std::move(api_package_id);
  }
  void set_api_build_id(const std::string& api_build_id) {
    api_build_id_ = api_build_id;
  }
  void set_api_build_id(std::string&& api_build_id) noexcept {
    api_build_id_ = std::move(api_build_id);
  }
  void set_api_tag(const std::string& api_tag) { api_tag_ = api_tag; }
  void set_api_tag(std::string&& api_tag) noexcept {
    api_tag_ = std::move(api_tag);
  }
  void set_api_category_id(const std::string& api_category_id) {
    api_category_id_ = api_category_id;
  }
  void set_api_category_id(std::string&& api_category_id) noexcept {
    api_category_id_ = std::move(api_category_id);
  }
  void set_api_matching_field(const std::string& api_matching_field) {
    api_matching_field_ = api_matching_field;
  }
  void set_api_matching_field(std::string&& api_matching_field) noexcept {
    api_matching_field_ = std::move(api_matching_field);
  }

  /**
   * @brief Validate data validity
   * @return Returns true if validation succeeds, otherwise false
   */
  bool is_valid() const noexcept;

 private:
  Quaton::ChunkInfo chunk_info_;        ///< Data chunk information
  ManifestMetadata manifest_metadata_;  ///< Manifest information
  bool found_ = true;                   ///< Whether found
  int return_code_ = 0;                 ///< Return code
  std::string return_message_;          ///< Return message
  std::string api_package_id_;          ///< API package ID
  std::string api_build_id_;            ///< API build ID
  std::string api_tag_;                 ///< API tag
  std::string api_category_id_;         ///< API category ID
  std::string api_matching_field_;      ///< API matching field
};

// Compile-time check
static_assert(std::is_nothrow_move_constructible_v<ChunkManifestPair>,
              "ChunkManifestPair should be nothrow move constructible");

}  // namespace Quaton

#endif  // QUATON_CHUNK_MANIFEST_PAIR_H_
