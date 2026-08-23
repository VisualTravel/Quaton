#include "quaton/utils/compression.h"

#include <zstd.h>

#include <fstream>
#include <stdexcept>

#include "quaton/logger.h"
#include "quaton/utils/path_helper.h"

QUATON_NAMESPACE_BEGIN

std::vector<uint8_t> CompressionUtils::DecompressZstd(
    const std::vector<uint8_t>& compressed_data,
    const std::string& output_dir) {
  if (compressed_data.empty()) {
    return compressed_data;  // No decompression needed, return directly
  }

  LOG_INFO("Decompressing data with Zstandard, compressed size: %zu bytes",
           compressed_data.size());

  // Get decompressed size
  unsigned long long decompressed_size =
      ZSTD_getFrameContentSize(compressed_data.data(), compressed_data.size());

  if (decompressed_size == ZSTD_CONTENTSIZE_ERROR) {
    throw std::runtime_error("Invalid Zstandard compressed data");
  }

  if (decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
    // Use a reasonable initial size if size cannot be determined
    decompressed_size = compressed_data.size() * 4;
  }

  std::vector<uint8_t> decompressed_data;
  decompressed_data.resize(decompressed_size);

  // Decompress data
  size_t actual_size = ZSTD_decompress(decompressed_data.data(),
                                       decompressed_data.size(),
                                       compressed_data.data(),
                                       compressed_data.size());

  if (ZSTD_isError(actual_size)) {
    throw std::runtime_error(std::string("Zstandard decompression failed: ") +
                             ZSTD_getErrorName(actual_size));
  }

  decompressed_data.resize(actual_size);
  LOG_INFO("Decompressed data, size: %zu bytes", actual_size);

  // Save decompressed file
  if (!output_dir.empty()) {
    std::string output_path = output_dir + "/decompressed_data.bin";
    PathHelper::CreateDirectoryRecursive(output_dir);

    std::ofstream out_file(output_path, std::ios::binary);
    if (out_file.is_open()) {
      out_file.write(reinterpret_cast<const char*>(decompressed_data.data()),
                     decompressed_data.size());
      out_file.close();
      LOG_INFO("Saved decompressed data to: %s", output_path.c_str());
    } else {
      LOG_WARN("Failed to save decompressed data to: %s", output_path.c_str());
    }
  }

  return decompressed_data;
}

std::vector<uint8_t> CompressionUtils::CompressZstd(
    const std::vector<uint8_t>& data, int compression_level) {
  if (data.empty()) {
    return data;
  }

  // Get maximum compressed size
  size_t max_compressed_size = ZSTD_compressBound(data.size());
  std::vector<uint8_t> compressed_data(max_compressed_size);

  // Compress data
  size_t compressed_size = ZSTD_compress(compressed_data.data(),
                                         compressed_data.size(),
                                         data.data(),
                                         data.size(),
                                         compression_level);

  if (ZSTD_isError(compressed_size)) {
    throw std::runtime_error(std::string("Zstandard compression failed: ") +
                             ZSTD_getErrorName(compressed_size));
  }

  compressed_data.resize(compressed_size);
  return compressed_data;
}

bool CompressionUtils::IsZstdCompressed(const std::vector<uint8_t>& data) {
  if (data.size() < 4) {
    return false;
  }

  // Zstd magic number: 0xFD2FB528
  return data[0] == 0x28 && data[1] == 0xB5 && data[2] == 0x2F &&
         data[3] == 0xFD;
}

size_t CompressionUtils::GetDecompressedSize(
    const std::vector<uint8_t>& compressed_data) {
  if (compressed_data.empty()) {
    return 0;
  }

  unsigned long long size =
      ZSTD_getFrameContentSize(compressed_data.data(), compressed_data.size());

  if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN) {
    return 0;
  }

  return static_cast<size_t>(size);
}

QUATON_NAMESPACE_END
