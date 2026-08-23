#include "quaton/manifest/chunk_manifest_pair.h"

#include <stdexcept>

namespace Quaton {

ChunkManifestPair::ChunkManifestPair(Quaton::ChunkInfo chunk_info,
                                     ManifestMetadata manifest_metadata)
    : chunk_info_(std::move(chunk_info)),
      manifest_metadata_(std::move(manifest_metadata)) {
  if (!manifest_metadata_.is_valid()) {
    throw std::invalid_argument("Invalid manifest metadata provided");
  }
}

ChunkManifestPair& ChunkManifestPair::operator=(
    ChunkManifestPair&& other) noexcept {
  if (this != &other) {
    manifest_metadata_ = std::move(other.manifest_metadata_);
    chunk_info_ = std::move(other.chunk_info_);
    found_ = other.found_;
    return_code_ = other.return_code_;
    return_message_ = std::move(other.return_message_);
    api_package_id_ = std::move(other.api_package_id_);
    api_build_id_ = std::move(other.api_build_id_);
    api_tag_ = std::move(other.api_tag_);
    api_category_id_ = std::move(other.api_category_id_);
    api_matching_field_ = std::move(other.api_matching_field_);
  }
  return *this;
}

}  // namespace Quaton
