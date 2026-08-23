#ifndef QUATON_MANIFEST_TYPES_H_
#define QUATON_MANIFEST_TYPES_H_
#pragma once

#include <cstdint>
#include <string>

namespace Quaton {

// ============================================================================
// Constants
// ============================================================================

/// @brief Validate allowed maximum data size
constexpr int64_t kMaxDataSize = 10LL * 1024 * 1024 * 1024;  // 10GB

// ============================================================================
// Basic types
// ============================================================================

/**
 * @brief Manifest file metadata containing identifier and size information
 */
class ManifestFileMetadata {
 public:
  ManifestFileMetadata() = default;
  ManifestFileMetadata(std::string identifier,
                       std::string checksum_value,
                       int64_t compressed_bytes,
                       int64_t uncompressed_bytes);

  // Copy constructor and assignment operator
  ManifestFileMetadata(const ManifestFileMetadata& other) = default;
  ManifestFileMetadata& operator=(const ManifestFileMetadata& other) = default;

  // Move constructor and assignment operator
  ManifestFileMetadata(ManifestFileMetadata&& other) noexcept = default;
  ManifestFileMetadata& operator=(ManifestFileMetadata&& other) noexcept =
      default;

  // Accessors
  const std::string& identifier() const { return identifier_; }
  const std::string& checksum() const { return checksum_; }
  int64_t compressed_size_bytes() const { return compressed_size_bytes_; }
  int64_t uncompressed_size_bytes() const { return uncompressed_size_bytes_; }

  // Modifiers
  void set_identifier(std::string identifier) {
    identifier_ = std::move(identifier);
  }
  void set_checksum(std::string checksum) { checksum_ = std::move(checksum); }
  void set_compressed_size_bytes(int64_t size) {
    compressed_size_bytes_ = size;
  }
  void set_uncompressed_size_bytes(int64_t size) {
    uncompressed_size_bytes_ = size;
  }

  /// @brief Validate metadata completeness
  /// @return Returns true if all fields are valid, otherwise false
  bool is_valid() const;

  /// @brief Generate debug string representation
  /// @return Debug string containing all field values
  std::string to_debug_string() const;

 private:
  std::string identifier_;
  std::string checksum_;
  int64_t compressed_size_bytes_ = 0;
  int64_t uncompressed_size_bytes_ = 0;
};

/**
 * @brief Manifest URL endpoint configuration with security settings
 */
class ManifestUrlConfiguration {
 public:
  ManifestUrlConfiguration() = default;
  ManifestUrlConfiguration(std::string password,
                           std::string url_prefix,
                           std::string url_suffix,
                           bool is_encrypted,
                           bool is_compressed);

  // Copy constructor and assignment operator
  ManifestUrlConfiguration(const ManifestUrlConfiguration& other) = default;
  ManifestUrlConfiguration& operator=(const ManifestUrlConfiguration& other) =
      default;

  // Move constructor and assignment operator
  ManifestUrlConfiguration(ManifestUrlConfiguration&& other) noexcept = default;
  ManifestUrlConfiguration& operator=(
      ManifestUrlConfiguration&& other) noexcept = default;

  // Accessors
  const std::string& password() const { return password_; }
  const std::string& url_prefix() const { return url_prefix_; }
  const std::string& url_suffix() const { return url_suffix_; }
  bool is_encrypted() const { return is_encrypted_; }
  bool is_compressed() const { return is_compressed_; }

  // Modifiers
  void set_password(std::string password) { password_ = std::move(password); }
  void set_url_prefix(std::string prefix) { url_prefix_ = std::move(prefix); }
  void set_url_suffix(std::string suffix) { url_suffix_ = std::move(suffix); }
  void set_encrypted(bool encrypted) { is_encrypted_ = encrypted; }
  void set_compressed(bool compressed) { is_compressed_ = compressed; }

  /// @brief Construct complete URL from prefix and suffix
  /// @return Complete URL string
  std::string construct_full_url() const;

  /// @brief Check if password is set
  /// @return Returns true if password is set, otherwise false
  bool has_password() const;

 private:
  std::string password_;
  std::string url_prefix_;
  std::string url_suffix_;
  bool is_encrypted_ = false;
  bool is_compressed_ = false;
};

/**
 * @brief Statistics about manifest chunks and files
 */
class ManifestChunkStatistics {
 public:
  ManifestChunkStatistics() = default;
  ManifestChunkStatistics(int64_t compressed_bytes,
                          int64_t uncompressed_bytes,
                          int file_quantity,
                          int chunk_quantity);

  // Copy constructor and assignment operator
  ManifestChunkStatistics(const ManifestChunkStatistics& other) = default;
  ManifestChunkStatistics& operator=(const ManifestChunkStatistics& other) =
      default;

  // Move constructor and assignment operator
  ManifestChunkStatistics(ManifestChunkStatistics&& other) noexcept = default;
  ManifestChunkStatistics& operator=(ManifestChunkStatistics&& other) noexcept =
      default;

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

  /// @brief Calculate compression ratio
  /// @return Compression ratio as double (compressed/uncompressed)
  double get_compression_ratio() const;

  /// @brief Validate that all counts are non-negative
  /// @return Returns true if all counts are valid, otherwise false
  bool has_valid_counts() const;

 private:
  int64_t compressed_size_bytes_ = 0;
  int64_t uncompressed_size_bytes_ = 0;
  int file_count_ = 0;
  int chunk_count_ = 0;
};

}  // namespace Quaton

#endif  // QUATON_MANIFEST_TYPES_H_
