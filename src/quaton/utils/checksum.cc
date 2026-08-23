#include "quaton/utils/checksum.h"

#include <xxhash.h>

#include <fstream>
#include <vector>

#include "quaton/logger.h"
#include "quaton/utils/md5.h"

namespace Quaton {
namespace ChecksumUtils {

std::string calculate_md5_file(const std::string& file_path) {
  std::ifstream file(file_path, std::ios::binary);
  if (!file.is_open()) {
    LOG_ERROR("Failed to open file for MD5 calculation: %s", file_path.c_str());
    return "";
  }

  Md5 md5;
  constexpr size_t buffer_size = 2 * 1024 * 1024;
  std::vector<char> buffer(buffer_size);

  while (file.read(buffer.data(), buffer_size) || file.gcount() > 0) {
    size_t bytes_read = file.gcount();
    if (bytes_read > 0) {
      md5.Update(buffer.data(), bytes_read);
    }

    if (file.eof()) {
      break;
    }

    if (file.fail() && !file.eof()) {
      LOG_ERROR("Error reading file for MD5 calculation: %s",
                file_path.c_str());
      return "";
    }
  }

  unsigned char md5_digest[Md5DigestLength];
  md5.Final(md5_digest);

  std::string result;
  result.reserve(Md5DigestLength * 2);
  char hex_buf[3];
  for (size_t i = 0; i < Md5DigestLength; ++i) {
    snprintf(hex_buf, sizeof(hex_buf), "%02x", md5_digest[i]);
    result.append(hex_buf, 2);
  }

  return result;
}

std::string calculate_md5_data(const uint8_t* data, size_t length) {
  if (!data || length == 0) {
    throw std::invalid_argument("Data buffer cannot be null or empty");
  }

  return Md5::HashData(data, length);
}

std::string calculate_xxhash_file(const std::string& file_path) {
  std::ifstream file(file_path, std::ios::binary);
  if (!file.is_open()) {
    LOG_ERROR("Failed to open file for XXHash calculation: %s",
              file_path.c_str());
    return "";
  }

  XXH64_state_t* state = XXH64_createState();
  if (!state) {
    LOG_ERROR("Failed to create XXHash state");
    return "";
  }

  if (XXH64_reset(state, 0) == XXH_ERROR) {
    LOG_ERROR("Failed to reset XXHash state");
    XXH64_freeState(state);
    return "";
  }

  // Use larger buffer to improve performance (2MB)
  constexpr size_t buffer_size = 2 * 1024 * 1024;
  std::vector<char> buffer(buffer_size);

  // Read file and update XXHash
  while (file.read(buffer.data(), buffer_size) || file.gcount() > 0) {
    size_t bytes_read = file.gcount();
    if (bytes_read > 0) {
      if (XXH64_update(state, buffer.data(), bytes_read) == XXH_ERROR) {
        LOG_ERROR("Failed to update XXHash");
        XXH64_freeState(state);
        return "";
      }
    }

    // Exit loop if end of file is reached
    if (file.eof()) {
      break;
    }

    // Report error if read error occurs (non-EOF)
    if (file.fail() && !file.eof()) {
      LOG_ERROR("Error reading file for XXHash calculation: %s",
                file_path.c_str());
      XXH64_freeState(state);
      return "";
    }
  }

  XXH64_hash_t hash = XXH64_digest(state);
  XXH64_freeState(state);

  std::string result;
  result.reserve(16);  // XXH64 produces 64-bit hash (16 hexadecimal characters)
  char hex_buf[17];
  snprintf(hex_buf, sizeof(hex_buf), "%016llx", hash);
  result = hex_buf;

  return result;
}

std::string calculate_xxhash_data(const uint8_t* data, size_t length) {
  if (!data || length == 0) {
    throw std::invalid_argument("Data buffer cannot be null or empty");
  }

  XXH64_hash_t hash = XXH64(data, length, 0);

  std::string result;
  result.reserve(16);  // XXH64 produces 64-bit hash (16 hexadecimal characters)
  char hex_buf[17];
  snprintf(hex_buf, sizeof(hex_buf), "%016llx", hash);
  result = hex_buf;

  return result;
}

bool verify_md5(const std::string& file_path, const std::string& expected_md5) {
  if (expected_md5.empty()) {
    return true;  // No verification needed
  }

  std::string actual_md5 = calculate_md5_file(file_path);
  if (actual_md5.empty()) {
    return false;
  }

  return actual_md5 == expected_md5;
}

bool verify_md5_data(const uint8_t* data,
                     size_t length,
                     const std::string& expected_md5) {
  if (expected_md5.empty()) {
    return true;  // No verification needed
  }

  std::string actual_md5 = calculate_md5_data(data, length);
  if (actual_md5.empty()) {
    return false;
  }

  return actual_md5 == expected_md5;
}

}  // namespace ChecksumUtils
}  // namespace Quaton
