#include "quaton/data_chunk.h"

#include <tuple>

namespace Quaton {

bool ChunkInfo::operator==(const ChunkInfo& other) const noexcept {
  return std::tie(base_url_,
                  chunk_count_,
                  file_count_,
                  total_size_,
                  compressed_total_size_,
                  uses_compression_) == std::tie(other.base_url_,
                                                 other.chunk_count_,
                                                 other.file_count_,
                                                 other.total_size_,
                                                 other.compressed_total_size_,
                                                 other.uses_compression_);
}

bool ChunkInfo::operator!=(const ChunkInfo& other) const noexcept {
  return !(*this == other);
}

ChunkInfo ChunkInfo::copy_with_new_base_url(
    const std::string& new_base_url) const {
  if (new_base_url.empty()) {
    throw std::invalid_argument("new_base_url cannot be empty");
  }
  ChunkInfo copy = *this;
  copy.base_url_ = new_base_url;
  return copy;
}

}  // namespace Quaton