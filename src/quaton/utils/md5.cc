#include "quaton/utils/md5.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace Quaton {

namespace {
// RFC 1321 round constants.
constexpr std::uint32_t kRoundConstants[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

constexpr int kShift[64] = {7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,
                            12, 17, 22, 5,  9,  14, 20, 5,  9,  14, 20, 5,  9,
                            14, 20, 5,  9,  14, 20, 4,  11, 16, 23, 4,  11, 16,
                            23, 4,  11, 16, 23, 4,  11, 16, 23, 6,  10, 15, 21,
                            6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21};

// Message-index order per round group.
constexpr int kIndex[64] = {0,  1,  2,  3, 4, 5,  6,  7,  8,  9,  10, 11, 12,
                            13, 14, 15, 1, 6, 11, 0,  5,  10, 15, 4,  9,  14,
                            3,  8,  13, 2, 7, 12, 5,  8,  11, 14, 1,  4,  7,
                            10, 13, 0,  3, 6, 9,  12, 15, 2,  0,  7,  14, 5,
                            12, 3,  10, 1, 8, 15, 6,  13, 4,  11, 2,  9};

constexpr std::uint32_t RotateLeft(std::uint32_t value, int bits) {
  return (value << bits) | (value >> (32 - bits));
}

constexpr std::uint32_t Load32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}
}  // namespace

Md5::Md5() : bit_count_(0), buffer_len_(0) {
  state_[0] = 0x67452301;
  state_[1] = 0xefcdab89;
  state_[2] = 0x98badcfe;
  state_[3] = 0x10325476;
}

void Md5::Transform(std::uint32_t state[4], const std::uint8_t block[64]) {
  std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  std::uint32_t m[16];
  for (int i = 0; i < 16; ++i) {
    m[i] = Load32(block + i * 4);
  }

  for (int i = 0; i < 64; ++i) {
    std::uint32_t f;
    if (i < 16) {
      f = (b & c) | (~b & d);
    } else if (i < 32) {
      f = (d & b) | (~d & c);
    } else if (i < 48) {
      f = b ^ c ^ d;
    } else {
      f = c ^ (b | ~d);
    }
    f = f + a + kRoundConstants[i] + m[kIndex[i]];
    a = d;
    d = c;
    c = b;
    b = b + RotateLeft(f, kShift[i]);
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
}

void Md5::ProcessBlock(const std::uint8_t* block) {
  Transform(state_, block);
}

void Md5::Update(const void* data, size_t length) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  bit_count_ += static_cast<std::uint64_t>(length) * 8;

  while (length > 0) {
    size_t to_copy = 64 - buffer_len_;
    if (to_copy > length) {
      to_copy = length;
    }
    std::memcpy(buffer_ + buffer_len_, bytes, to_copy);
    buffer_len_ += to_copy;
    bytes += to_copy;
    length -= to_copy;

    if (buffer_len_ == 64) {
      ProcessBlock(buffer_);
      buffer_len_ = 0;
    }
  }
}

void Md5::Final(std::uint8_t digest[Md5DigestLength]) {
  // RFC 1321 requires the original message bit length (excluding padding).
  std::uint64_t count = bit_count_;

  // Append the 0x80 padding byte.
  std::uint8_t padding = 0x80;
  Update(&padding, 1);

  // Pad with zeros until 56 bytes mod 64.
  while (buffer_len_ != 56) {
    std::uint8_t zero = 0;
    Update(&zero, 1);
  }

  // Append the 64-bit little-endian bit count.
  std::uint8_t length_bits[8];
  for (int i = 0; i < 8; ++i) {
    length_bits[i] = static_cast<std::uint8_t>(count & 0xff);
    count >>= 8;
  }
  Update(length_bits, sizeof(length_bits));

  for (int i = 0; i < 4; ++i) {
    digest[i * 4] = static_cast<std::uint8_t>(state_[i] & 0xff);
    digest[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xff);
    digest[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xff);
    digest[i * 4 + 3] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xff);
  }
}

std::string Md5::HashFile(const std::string& file_path) {
  std::ifstream file(file_path, std::ios::binary);
  if (!file.is_open()) {
    return "";
  }

  Md5 md5;
  std::vector<char> buffer(64 * 1024);
  while (
      file.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
      file.gcount() > 0) {
    md5.Update(buffer.data(), static_cast<size_t>(file.gcount()));
    if (file.eof()) {
      break;
    }
  }

  unsigned char digest[Md5DigestLength];
  md5.Final(digest);

  std::string result;
  result.reserve(Md5DigestLength * 2);
  for (size_t i = 0; i < Md5DigestLength; ++i) {
    char hex_buf[3];
    std::snprintf(hex_buf, sizeof(hex_buf), "%02x", digest[i]);
    result.append(hex_buf, 2);
  }
  return result;
}

std::string Md5::HashData(const void* data, size_t length) {
  Md5 md5;
  md5.Update(data, length);

  unsigned char digest[Md5DigestLength];
  md5.Final(digest);

  std::string result;
  result.reserve(Md5DigestLength * 2);
  for (size_t i = 0; i < Md5DigestLength; ++i) {
    char hex_buf[3];
    std::snprintf(hex_buf, sizeof(hex_buf), "%02x", digest[i]);
    result.append(hex_buf, 2);
  }
  return result;
}

}  // namespace Quaton
