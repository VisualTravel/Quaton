#ifndef UTILS_MD5_H
#define UTILS_MD5_H
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace Quaton {

// Lightweight MD5 implementation (RFC 1321) with no external dependencies.
// Used as a drop-in replacement for OpenSSL's MD5 so the crypto library is not
// required just for checksum computation.

constexpr size_t Md5DigestLength = 16;

class Md5 {
 public:
  Md5();

  void Update(const void* data, size_t length);
  void Final(std::uint8_t digest[Md5DigestLength]);

  // Convenience helpers returning lowercase hex strings.
  static std::string HashFile(const std::string& file_path);
  static std::string HashData(const void* data, size_t length);

 private:
  void ProcessBlock(const std::uint8_t* block);
  void Transform(std::uint32_t state[4], const std::uint8_t block[64]);

  std::uint32_t state_[4];
  std::uint64_t bit_count_;
  unsigned char buffer_[64];
  size_t buffer_len_;
};

}  // namespace Quaton

#endif  // UTILS_MD5_H
