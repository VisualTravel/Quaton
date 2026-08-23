#ifndef QUATON_MANIFEST_METADATA_H_
#define QUATON_MANIFEST_METADATA_H_
#pragma once

#include <string>

namespace Quaton {

// ============================================================================
// ManifestMetadata - Manifest information class
// ============================================================================

/**
 * @class ManifestMetadata
 * @brief Quaton manifest unit data class
 *
 * Contains basic manifest file information and URL construction, provides RAII
 * resource management
 */
class ManifestMetadata {
 public:
  /**
   * @brief Default constructor
   */
  ManifestMetadata() noexcept = default;

  /**
   * @brief Constructor
   * @param base_url Manifest base URL
   * @param checksum_md5 MD5 checksum
   * @param id Manifest ID
   * @param use_compression Whether to use compression
   * @param size Manifest size
   * @param compressed_size Compressed size, default 0
   * @throws std::invalid_argument If parameters are invalid
   */
  ManifestMetadata(std::string base_url,
                   std::string checksum_md5,
                   std::string id,
                   bool use_compression,
                   long size,
                   long compressed_size = 0);

  /**
   * @brief Move constructor
   * @param other Instance to move
   */
  ManifestMetadata(ManifestMetadata&& other) noexcept;

  /**
   * @brief Move assignment operator
   * @param other Instance to move
   * @return Reference to self
   */
  ManifestMetadata& operator=(ManifestMetadata&& other) noexcept;

  /**
   * @brief Copy constructor
   * @param other Instance to copy
   */
  ManifestMetadata(const ManifestMetadata& other) = default;

  /**
   * @brief Copy assignment operator
   * @param other Instance to copy
   * @return Reference to self
   */
  ManifestMetadata& operator=(const ManifestMetadata& other) = default;

  /**
   * @brief Get manifest file URL
   * @return Complete manifest file URL
   */
  std::string get_file_url() const;

  /**
   * @brief Get manifest file name
   * @return Manifest file name
   */
  std::string get_file_name() const noexcept { return id_; }

  /**
   * @brief Validate metadata validity
   * @return Returns true if validation succeeds, otherwise false
   */
  bool is_valid() const noexcept;

  /**
   * @brief Get base URL
   * @return Constant reference to base URL
   */
  const std::string& get_base_url() const noexcept { return base_url_; }

  /**
   * @brief Set base URL
   * @param base_url Base URL
   */
  void set_base_url(const std::string& base_url) { base_url_ = base_url; }

  /**
   * @brief Set base URL (move semantics)
   * @param base_url Base URL
   */
  void set_base_url(std::string&& base_url) noexcept {
    base_url_ = std::move(base_url);
  }

  /**
   * @brief Get MD5 checksum
   * @return Constant reference to MD5 checksum value
   */
  const std::string& get_checksum_md5() const noexcept { return checksum_md5_; }

  /**
   * @brief Get ID
   * @return Constant reference to ID
   */
  const std::string& get_id() const noexcept { return id_; }

  /**
   * @brief Check if compression is used
   * @return Whether compression is used
   */
  bool get_use_compression() const noexcept { return use_compression_; }

  /**
   * @brief Get size
   * @return Size
   */
  long get_size() const noexcept { return size_; }

  /**
   * @brief Get compressed size
   * @return Compressed size
   */
  long get_compressed_size() const noexcept { return compressed_size_; }

 private:
  std::string base_url_;      ///< Manifest base URL
  std::string id_;            ///< Manifest ID
  std::string checksum_md5_;  ///< Manifest MD5 checksum
  bool use_compression_;      ///< Whether to use compression
  long size_;                 ///< Manifest size
  long compressed_size_;      ///< Compressed size
};

/**
 * @brief Create manifest metadata instance
 * @param base_url Manifest base URL
 * @param checksum_md5 MD5 checksum
 * @param id Manifest ID
 * @param use_compression Whether to use compression
 * @param size Manifest size
 * @param compressed_size Compressed size, default 0
 * @return ManifestMetadata instance
 * @throws std::invalid_argument If parameters are invalid
 */
ManifestMetadata create_manifest_metadata(const std::string& base_url,
                                          const std::string& checksum_md5,
                                          const std::string& id,
                                          bool use_compression,
                                          long size,
                                          long compressed_size = 0);

// Compile-time check
static_assert(std::is_nothrow_move_constructible_v<ManifestMetadata>,
              "ManifestMetadata should be nothrow move constructible");

}  // namespace Quaton

#endif  // QUATON_MANIFEST_METADATA_H_
