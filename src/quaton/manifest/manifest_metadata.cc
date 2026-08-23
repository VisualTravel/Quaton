#include "quaton/manifest/manifest_metadata.h"

#include <stdexcept>

namespace Quaton {

ManifestMetadata::ManifestMetadata(std::string base_url,
                                   std::string checksum_md5,
                                   std::string id,
                                   bool use_compression,
                                   long size,
                                   long compressed_size)
    : base_url_(std::move(base_url)),
      checksum_md5_(std::move(checksum_md5)),
      id_(std::move(id)),
      use_compression_(use_compression),
      size_(size),
      compressed_size_(compressed_size) {
  if (base_url_.empty()) {
    throw std::invalid_argument("base_url cannot be empty");
  }
  if (checksum_md5_.empty()) {
    throw std::invalid_argument("checksum_md5 cannot be empty");
  }
  if (id_.empty()) {
    throw std::invalid_argument("id cannot be empty");
  }
  if (size_ <= 0) {
    throw std::invalid_argument("size must be positive");
  }
  if (compressed_size_ < 0) {
    throw std::invalid_argument("compressed_size cannot be negative");
  }
}

ManifestMetadata::ManifestMetadata(ManifestMetadata&& other) noexcept
    : base_url_(std::move(other.base_url_)),
      checksum_md5_(std::move(other.checksum_md5_)),
      id_(std::move(other.id_)),
      use_compression_(other.use_compression_),
      size_(other.size_),
      compressed_size_(other.compressed_size_) {
}

ManifestMetadata& ManifestMetadata::operator=(
    ManifestMetadata&& other) noexcept {
  if (this != &other) {
    base_url_ = std::move(other.base_url_);
    checksum_md5_ = std::move(other.checksum_md5_);
    id_ = std::move(other.id_);
    use_compression_ = other.use_compression_;
    size_ = other.size_;
    compressed_size_ = other.compressed_size_;
  }
  return *this;
}

ManifestMetadata create_manifest_metadata(const std::string& base_url,
                                          const std::string& checksum_md5,
                                          const std::string& id,
                                          bool use_compression,
                                          long size,
                                          long compressed_size) {
  return ManifestMetadata(
      base_url, checksum_md5, id, use_compression, size, compressed_size);
}

std::string ManifestMetadata::get_file_url() const {
  if (base_url_.empty() || id_.empty()) {
    throw std::invalid_argument(
        "Invalid manifest metadata: base_url or id is empty");
  }

  std::string base_url = base_url_;

  if (!base_url.empty() && base_url.back() == '/') {
    base_url.pop_back();
  }

  return base_url + "/" + id_;
}

bool ManifestMetadata::is_valid() const noexcept {
  return !base_url_.empty() && !id_.empty() && !checksum_md5_.empty() &&
         size_ > 0 && compressed_size_ >= 0;
}

}  // namespace Quaton
