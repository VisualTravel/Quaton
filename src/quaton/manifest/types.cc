#include "quaton/manifest/types.h"

#include <sstream>
#include <stdexcept>

namespace Quaton {

// ============================================================================
// ManifestFileMetadata implementation
// ============================================================================

ManifestFileMetadata::ManifestFileMetadata(std::string identifier,
                                           std::string checksum_value,
                                           int64_t compressed_bytes,
                                           int64_t uncompressed_bytes)
    : identifier_(std::move(identifier)),
      checksum_(std::move(checksum_value)),
      compressed_size_bytes_(compressed_bytes),
      uncompressed_size_bytes_(uncompressed_bytes) {
  if (compressed_size_bytes_ < 0) {
    throw std::invalid_argument("Compressed size cannot be negative");
  }
  if (uncompressed_size_bytes_ < 0) {
    throw std::invalid_argument("Uncompressed size cannot be negative");
  }
}

bool ManifestFileMetadata::is_valid() const {
  return !identifier_.empty() && !checksum_.empty() &&
         compressed_size_bytes_ >= 0 && uncompressed_size_bytes_ >= 0;
}

std::string ManifestFileMetadata::to_debug_string() const {
  std::ostringstream oss;
  oss << "ManifestFileMetadata{"
      << "id='" << identifier_ << "', "
      << "checksum='" << checksum_ << "', "
      << "compressed=" << compressed_size_bytes_ << ", "
      << "uncompressed=" << uncompressed_size_bytes_ << "}";
  return oss.str();
}

// ============================================================================
// ManifestUrlConfiguration implementation
// ============================================================================

ManifestUrlConfiguration::ManifestUrlConfiguration(std::string password,
                                                   std::string url_prefix,
                                                   std::string url_suffix,
                                                   bool is_encrypted,
                                                   bool is_compressed)
    : password_(std::move(password)),
      url_prefix_(std::move(url_prefix)),
      url_suffix_(std::move(url_suffix)),
      is_encrypted_(is_encrypted),
      is_compressed_(is_compressed) {
}

std::string ManifestUrlConfiguration::construct_full_url() const {
  std::string full_url = url_prefix_;
  if (!url_suffix_.empty()) {
    if (!full_url.empty() && full_url.back() != '/') {
      full_url += '/';
    }
    full_url += url_suffix_;
  }
  return full_url;
}

bool ManifestUrlConfiguration::has_password() const {
  return !password_.empty();
}

// ============================================================================
// ManifestChunkStatistics implementation
// ============================================================================

ManifestChunkStatistics::ManifestChunkStatistics(int64_t compressed_bytes,
                                                 int64_t uncompressed_bytes,
                                                 int file_quantity,
                                                 int chunk_quantity)
    : compressed_size_bytes_(compressed_bytes),
      uncompressed_size_bytes_(uncompressed_bytes),
      file_count_(file_quantity),
      chunk_count_(chunk_quantity) {
}

double ManifestChunkStatistics::get_compression_ratio() const {
  if (uncompressed_size_bytes_ == 0) return 0.0;
  return static_cast<double>(compressed_size_bytes_) / uncompressed_size_bytes_;
}

bool ManifestChunkStatistics::has_valid_counts() const {
  return file_count_ >= 0 && chunk_count_ >= 0 && compressed_size_bytes_ >= 0 &&
         uncompressed_size_bytes_ >= 0;
}

}  // namespace Quaton
