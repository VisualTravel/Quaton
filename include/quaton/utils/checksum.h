#ifndef UTILS_CHECKSUM_H
#define UTILS_CHECKSUM_H
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "quaton/quaton_global.h"

QUATON_NAMESPACE_BEGIN
namespace ChecksumUtils {

/**
 * @brief Calculate MD5 hash of file
 * @param file_path File path
 * @return MD5 hash as lowercase hexadecimal string, empty string on failure
 */
QUATON_API std::string calculate_md5_file(const std::string& file_path);

/**
 * @brief Calculate MD5 hash of data
 * @param data Data pointer
 * @param length Data length
 * @return MD5 hash as lowercase hexadecimal string, empty string on failure
 * @throws std::invalid_argument if data is null or length is 0
 */
QUATON_API std::string calculate_md5_data(const uint8_t* data, size_t length);

/**
 * @brief Calculate XXHash64 hash of file
 * @param file_path File path
 * @return XXHash64 hash as lowercase hexadecimal string, empty string on
 * failure
 */
QUATON_API std::string calculate_xxhash_file(const std::string& file_path);

/**
 * @brief Calculate XXHash64 hash of data
 * @param data Data pointer
 * @param length Data length
 * @return XXHash64 hash as lowercase hexadecimal string, empty string on
 * failure
 * @throws std::invalid_argument if data is null or length is 0
 */
QUATON_API std::string calculate_xxhash_data(const uint8_t* data,
                                             size_t length);

/**
 * @brief Verify file MD5 checksum
 * @param file_path File path
 * @param expected_md5 Expected MD5 hash
 * @return true if matches, false otherwise
 */
QUATON_API bool verify_md5(const std::string& file_path,
                           const std::string& expected_md5);

/**
 * @brief Verify data MD5 checksum
 * @param data Data pointer
 * @param length Data length
 * @param expected_md5 Expected MD5 hash
 * @return true if matches, false otherwise
 * @throws std::invalid_argument if data is null or length is 0
 */
QUATON_API bool verify_md5_data(const uint8_t* data,
                                size_t length,
                                const std::string& expected_md5);

}  // namespace ChecksumUtils
QUATON_NAMESPACE_END

#endif  // UTILS_CHECKSUM_H
