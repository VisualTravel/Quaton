#ifndef DATA_CHUNK_H
#define DATA_CHUNK_H

#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace Quaton {

// ============================================================================
// Constants
// ============================================================================

// Maximum allowed chunk size for validation
constexpr int64_t kMaxChunkSize = 10LL * 1024 * 1024 * 1024;  // 10GB

// ============================================================================
// Data structures
// ============================================================================

/**
 * @struct DataChunk
 * @brief Represents a data chunk in the Quaton download system
 */
struct DataChunk {
  // old_offset_ = -1 marks "no old file offset reference"
  DataChunk() : old_offset_(-1), offset_(0), container_offset_(0) {}

  // Move constructor and assignment operator
  DataChunk(DataChunk&& other) noexcept = default;
  DataChunk& operator=(DataChunk&& other) noexcept = default;

  // Copy constructor and assignment operator (explicitly defaulted for
  // compatibility)
  DataChunk(const DataChunk&) = default;
  DataChunk& operator=(const DataChunk&) = default;

  std::string name_;  // Container file name (chunk_NNNN.bin)
  std::vector<uint8_t> decompressed_hash_;  // Hash of decompressed data
  int64_t old_offset_;                      // Offset in old file
  int64_t offset_;                          // Offset within the asset file
  int64_t container_offset_;   // Offset of the block within container
  int64_t size_;               // Compressed chunk size
  int64_t decompressed_size_;  // Decompressed size
};

/**
 * @class ChunkBranch
 * @brief Represents branch information for chunks
 */
class ChunkBranch {
 public:
  std::string name_;                      // Branch name
  std::string version_;                   // Branch version
  std::vector<std::string> chunk_names_;  // List of chunk names

  // Default constructor
  ChunkBranch() = default;

  // Constructor with name and version
  // @param branch_name Branch name
  // @param branch_version Branch version
  ChunkBranch(const std::string& branch_name, const std::string& branch_version)
      : name_(branch_name), version_(branch_version) {}

  ChunkBranch(ChunkBranch&& other) noexcept = default;
  ChunkBranch& operator=(ChunkBranch&& other) noexcept = default;

  ChunkBranch(const ChunkBranch&) = default;
  ChunkBranch& operator=(const ChunkBranch&) = default;
};

/**
 * @class ChunkInfo
 * @brief Contains basic information for chunk downloads
 */
class ChunkInfo {
 public:
  // Default constructor
  ChunkInfo() = default;

  // Constructor with parameters
  // @param base_url Base URL for chunks
  // @param chunk_count Number of chunks
  // @param file_count Number of files
  // @param uses_compression Whether compression is used
  // @param total_size Total size
  // @param compressed_total_size Compressed total size
  ChunkInfo(std::string base_url,
            int chunk_count,
            int file_count,
            bool uses_compression,
            int64_t total_size,
            int64_t compressed_total_size)
      : base_url_(std::move(base_url)),
        chunk_count_(chunk_count),
        file_count_(file_count),
        uses_compression_(uses_compression),
        total_size_(total_size),
        compressed_total_size_(compressed_total_size) {
    if (this->base_url_.empty()) {
      throw std::invalid_argument("base_url cannot be empty");
    }
    if (total_size_ < 0 || compressed_total_size_ < 0) {
      throw std::invalid_argument("Size cannot be negative");
    }
    if (chunk_count_ < 0 || file_count_ < 0) {
      throw std::invalid_argument("Count cannot be negative");
    }
  }

  ChunkInfo(ChunkInfo&& other) noexcept = default;
  ChunkInfo& operator=(ChunkInfo&& other) noexcept = default;

  ChunkInfo(const ChunkInfo&) = default;
  ChunkInfo& operator=(const ChunkInfo&) = default;

  // Equality operator
  // @param other Another ChunkInfo object
  // @return true if equal, false otherwise
  bool operator==(const ChunkInfo& other) const noexcept;

  // Inequality operator
  // @param other Another ChunkInfo object
  // @return true if not equal, false otherwise
  bool operator!=(const ChunkInfo& other) const noexcept;

  // Create a copy with new base URL
  // @param new_base_url New base URL
  // @return New ChunkInfo object
  // @throws std::invalid_argument if new_base_url is empty
  ChunkInfo copy_with_new_base_url(const std::string& new_base_url) const;

  // Get base URL
  // @return Base URL
  const std::string& get_base_url() const noexcept { return base_url_; }

  // Set base URL
  // @param base_url Base URL
  void set_base_url(const std::string& base_url) { this->base_url_ = base_url; }

  // Set base URL (move semantics)
  // @param base_url Base URL
  void set_base_url(std::string&& base_url) noexcept {
    this->base_url_ = std::move(base_url);
  }

  std::string base_url_;           // Base URL for chunks
  int chunk_count_;                // Number of chunks
  int file_count_;                 // Number of files
  int64_t total_size_;             // Total size
  int64_t compressed_total_size_;  // Compressed total size
  bool uses_compression_;          // Whether compression is used
};

}  // namespace Quaton

// ============================================================================
// Compile-time checks
// ============================================================================

static_assert(std::is_nothrow_move_constructible_v<Quaton::DataChunk>,
              "DataChunk should be nothrow move constructible");
static_assert(std::is_nothrow_move_constructible_v<Quaton::ChunkBranch>,
              "ChunkBranch should be nothrow move constructible");
static_assert(std::is_nothrow_move_constructible_v<Quaton::ChunkInfo>,
              "ChunkInfo should be nothrow move constructible");

#endif  // DATA_CHUNK_H