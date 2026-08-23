#ifndef QUATON_UTILS_COMPRESSION_H_
#define QUATON_UTILS_COMPRESSION_H_
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "quaton/quaton_global.h"

QUATON_NAMESPACE_BEGIN

/**
 * @class CompressionUtils
 * @brief Utility class for compression operations
 */
class QUATON_API CompressionUtils {
 public:
  /**
   * @brief Decompress Zstandard compressed data
   * @param compressed_data Compressed data
   * @param output_dir Optional output directory for saving decompressed file
   * @return Decompressed data as vector<uint8_t>
   * @throws std::runtime_error if decompression fails
   */
  static std::vector<uint8_t> DecompressZstd(
      const std::vector<uint8_t>& compressed_data,
      const std::string& output_dir = "");

  /**
   * @brief Compress data using Zstandard
   * @param data Data to compress
   * @param compression_level Compression level (1-22, default 3)
   * @return Compressed data
   * @throws std::runtime_error if compression fails
   */
  static std::vector<uint8_t> CompressZstd(const std::vector<uint8_t>& data,
                                           int compression_level = 3);

  /**
   * @brief Check if data is Zstandard compressed
   * @param data Data to check
   * @return true if data has Zstd magic number
   */
  static bool IsZstdCompressed(const std::vector<uint8_t>& data);

  /**
   * @brief Get decompressed size from Zstd frame
   * @param compressed_data Compressed data
   * @return Decompressed size, or 0 if unknown
   */
  static size_t GetDecompressedSize(
      const std::vector<uint8_t>& compressed_data);

 private:
  CompressionUtils() = default;  // Static class, no instantiation
};

QUATON_NAMESPACE_END

#endif  // QUATON_UTILS_COMPRESSION_H_
