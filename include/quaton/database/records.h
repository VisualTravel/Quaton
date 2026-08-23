#ifndef QUATON_DATABASE_RECORDS_H
#define QUATON_DATABASE_RECORDS_H
#pragma once

#include <string>

namespace Quaton {

/**
 * @struct FileRecord
 * @brief File record structure for chunk.db storage
 */
struct FileRecord {
  std::string package_id;     ///< Package ID
  std::string branch;         ///< Branch name
  std::string build_id;       ///< Build ID
  std::string depot_id;       ///< Depot ID
  std::string file_id;        ///< File identifier
  std::string file_ver;       ///< File version
  std::string install_dir;    ///< Install directory
  std::string file_checksum;  ///< File XXHash checksum
  int file_status;            ///< File status (1=normal, 0=cached)

  FileRecord() : file_status(1) {}
};

/**
 * @struct ConfigRecord
 * @brief Configuration record for game version tracking
 */
struct ConfigRecord {
  int id = 0;                   ///< Record ID
  std::string local_version;    ///< Local game version
  std::string server_version;   ///< Server game version
  std::string local_build_id;   ///< Local build ID
  std::string server_build_id;  ///< Server build ID
  std::string branch;           ///< Game branch
  std::string package_id;       ///< Package identifier
  std::string depot_id;         ///< Depot identifier
  std::string matching_field;   ///< Matching field for updates
  std::string install_dir;      ///< Installation directory

  ConfigRecord() = default;
};

/**
 * @struct ManifestRecord
 * @brief Manifest record structure for chunk_manifest.db storage
 */
struct ManifestRecord {
  int id;                                      ///< Record ID
  std::string package_id;                      ///< Package ID
  std::string build_id;                        ///< Build ID
  std::string depot_id;                        ///< Depot ID
  std::string version;                         ///< Version
  std::string depot_manifest_id;               ///< Manifest ID
  std::string depot_manifest_checksum;         ///< Manifest checksum
  long long depot_manifest_compressed_size;    ///< Compressed size
  long long depot_manifest_uncompressed_size;  ///< Uncompressed size
  std::string depot_manifest_url_prefix;       ///< URL prefix
  int depot_manifest_encryption;               ///< Encryption flag
  std::string depot_manifest_password;         ///< Password
  int depot_manifest_compression;              ///< Compression flag
  std::string depot_manifest_md5;              ///< MD5 hash

  ManifestRecord()
      : id(0),
        depot_manifest_compressed_size(0),
        depot_manifest_uncompressed_size(0),
        depot_manifest_encryption(0),
        depot_manifest_compression(0) {}
};

/**
 * @struct FileValidationParams
 * @brief Parameters for file validation operations
 */
struct FileValidationParams {
  std::string file_path;      ///< Relative file path
  std::string install_dir;    ///< Installation directory
  std::string file_checksum;  ///< Expected file checksum (for database lookup)
  std::string package_id;     ///< Package ID
  std::string branch;         ///< Branch name
  std::string build_id;       ///< Build ID
  std::string depot_id;       ///< Depot ID
  std::string file_ver;       ///< File version

  FileValidationParams() = default;
};

}  // namespace Quaton

#endif  // QUATON_DATABASE_RECORDS_H
