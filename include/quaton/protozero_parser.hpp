#ifndef PROTOZERO_PARSER_HPP
#define PROTOZERO_PARSER_HPP
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstdint>
#include <cstring>  // for memcmp
#include <functional>
#include <protozero/pbf_reader.hpp>
#include <stdexcept>  // for std::invalid_argument, std::length_error
#include <string>
#include <type_traits>  // for static_assert
#include <vector>

namespace Quaton {
namespace ProtozeroParser {

// ============================================================================
// Constants
// ============================================================================

/// @brief Maximum data size allowed for parsing (100MB)
constexpr size_t kMaxDataSize = 100 * 1024 * 1024;

// ============================================================================
// Manifest parser structures
// ============================================================================

/**
 * @struct ManifestAssetChunk
 * @brief Chunk information parsed from manifest
 */
struct ManifestAssetChunk {
  std::string chunk_name;  // container file name (chunk_NNNN.bin)
  std::string chunk_decompressed_hash_md5;
  int64_t chunk_on_file_offset = 0;     // offset of the block within container
  int64_t chunk_size = 0;               // compressed size of the block
  int64_t chunk_size_decompressed = 0;  // decompressed size of the block
  int64_t asset_offset = 0;             // offset of the block within the asset
};

/**
 * @struct ManifestAsset
 * @brief Asset information parsed from manifest
 */
struct ManifestAsset {
  std::string asset_name;
  std::vector<ManifestAssetChunk> asset_chunks;
  int32_t asset_type = 0;
  int64_t asset_size = 0;
  std::string asset_hash_md5;

  // Move constructor and assignment for better performance
  ManifestAsset() = default;
  ManifestAsset(ManifestAsset&& other) noexcept = default;
  ManifestAsset& operator=(ManifestAsset&& other) noexcept = default;
};

/**
 * @brief Parse single asset chunk from protozero reader
 * @param reader Protozero reader positioned at chunk message
 * @return Parsed chunk information
 */
inline ManifestAssetChunk parse_manifest_asset_chunk(
    protozero::pbf_reader&& reader) {
  ManifestAssetChunk chunk;

  while (reader.next()) {
    switch (reader.tag()) {
      case 1:  // Chunk name
        chunk.chunk_name = reader.get_string();
        break;
      case 2:  // Chunk decompressed hash MD5
        chunk.chunk_decompressed_hash_md5 = reader.get_string();
        break;
      case 3:  // Chunk offset in file
        chunk.chunk_on_file_offset = reader.get_int64();
        break;
      case 4:  // Chunk size
        chunk.chunk_size = reader.get_int64();
        break;
      case 5:  // Chunk decompressed size
        chunk.chunk_size_decompressed = reader.get_int64();
        break;
      case 6:  // Asset offset (offset of the block within the asset)
        chunk.asset_offset = reader.get_int64();
        break;
      default:
        reader.skip();
        break;
    }
  }

  return chunk;
}

/**
 * @brief Parse single asset from protozero reader
 * @param reader Protozero reader positioned at asset message
 * @return Parsed asset information
 */
inline ManifestAsset parse_manifest_asset(protozero::pbf_reader&& reader) {
  ManifestAsset asset;

  while (reader.next()) {
    switch (reader.tag()) {
      case 1:  // Asset name
        asset.asset_name = reader.get_string();
        break;
      case 2:  // Asset chunks (repeated)
        asset.asset_chunks.push_back(
            parse_manifest_asset_chunk(reader.get_message()));
        break;
      case 3:  // Asset type
        asset.asset_type = reader.get_int32();
        break;
      case 4:  // Asset size
        asset.asset_size = reader.get_int64();
        break;
      case 5:  // Asset hash MD5
        asset.asset_hash_md5 = reader.get_string();
        break;
      default:
        reader.skip();
        break;
    }
  }

  return asset;
}

/**
 * @brief Parse each asset in entire manifest using callback
 * @param data Raw protobuf data
 * @param callback Function called for each parsed asset
 * @throws std::invalid_argument if data is empty or exceeds maximum size
 */
inline void parse_manifest(const std::vector<uint8_t>& data,
                           std::function<void(const ManifestAsset&)> callback) {
  if (data.empty()) {
    throw std::invalid_argument("Manifest data cannot be empty");
  }
  if (data.size() > kMaxDataSize) {
    throw std::length_error("Manifest data size exceeds maximum limit");
  }

  protozero::pbf_reader manifest(reinterpret_cast<const char*>(data.data()),
                                 data.size());

  while (manifest.next()) {
    if (manifest.tag() == 1) {  // Assets (repeated)
      ManifestAsset asset = parse_manifest_asset(manifest.get_message());
      callback(asset);
    } else {
      manifest.skip();
    }
  }
}

// ============================================================================
// Patch parser structures
// ============================================================================

/**
 * @struct PatchAssetChunk
 * @brief Parsed patch chunk information
 */
struct PatchAssetChunk {
  std::string patch_name;
  std::string version_tag;
  std::string build_id;
  int64_t patch_size = 0;
  std::string patch_md5;
  int64_t patch_offset = 0;
  int64_t patch_length = 0;
  std::string original_file_name;
  int64_t original_file_length = 0;
  std::string original_file_md5;
};

/**
 * @struct PatchAssetInfo
 * @brief Parsed patch asset information with version tag
 */
struct PatchAssetInfo {
  std::string version_tag;
  PatchAssetChunk chunk;
};

/**
 * @struct PatchAsset
 * @brief Parsed patch asset properties
 */
struct PatchAsset {
  std::string asset_name;
  int64_t asset_size = 0;
  std::string asset_hash_md5;
  std::vector<PatchAssetInfo> asset_infos;
};

/**
 * @struct UnusedAssetFile
 * @brief Parsed unused asset file
 */
struct UnusedAssetFile {
  std::string file_name;
  int64_t file_size = 0;
  std::string file_md5;
};

/**
 * @struct UnusedAssetInfo
 * @brief Container for unused asset files
 */
struct UnusedAssetInfo {
  std::vector<UnusedAssetFile> assets;
};

/**
 * @struct UnusedAssetProperty
 * @brief Unused asset properties with version tag
 */
struct UnusedAssetProperty {
  std::string version_tag;
  std::vector<UnusedAssetInfo> asset_infos;
};

/**
 * @brief Parse patch asset chunk using protozero reader
 */
inline PatchAssetChunk parse_patch_asset_chunk(protozero::pbf_reader&& reader) {
  PatchAssetChunk chunk;

  while (reader.next()) {
    switch (reader.tag()) {
      case 1:
        chunk.patch_name = reader.get_string();
        break;
      case 2:
        chunk.version_tag = reader.get_string();
        break;
      case 3:
        chunk.build_id = reader.get_string();
        break;
      case 4:
        chunk.patch_size = reader.get_int64();
        break;
      case 5:
        chunk.patch_md5 = reader.get_string();
        break;
      case 6:
        chunk.patch_offset = reader.get_int64();
        break;
      case 7:
        chunk.patch_length = reader.get_int64();
        break;
      case 8:
        chunk.original_file_name = reader.get_string();
        break;
      case 9:
        chunk.original_file_length = reader.get_int64();
        break;
      case 10:
        chunk.original_file_md5 = reader.get_string();
        break;
      default:
        reader.skip();
        break;
    }
  }

  return chunk;
}

/**
 * @brief Parse patch asset info using protozero reader
 */
inline PatchAssetInfo parse_patch_asset_info(protozero::pbf_reader&& reader) {
  PatchAssetInfo info;

  while (reader.next()) {
    switch (reader.tag()) {
      case 1:  // Version tag
        info.version_tag = reader.get_string();
        break;
      case 2:  // Chunk
        info.chunk = parse_patch_asset_chunk(reader.get_message());
        break;
      default:
        reader.skip();
        break;
    }
  }

  return info;
}

/**
 * @brief Parse patch asset using protozero reader
 */
inline PatchAsset parse_patch_asset(protozero::pbf_reader&& reader) {
  PatchAsset asset;

  while (reader.next()) {
    switch (reader.tag()) {
      case 1:  // Asset name
        asset.asset_name = reader.get_string();
        break;
      case 2:  // Asset size
        asset.asset_size = reader.get_int64();
        break;
      case 3:  // Asset hash MD5
        asset.asset_hash_md5 = reader.get_string();
        break;
      case 4:  // Asset info (repeated)
        asset.asset_infos.push_back(
            parse_patch_asset_info(reader.get_message()));
        break;
      default:
        reader.skip();
        break;
    }
  }

  return asset;
}

/**
 * @brief Parse unused asset file using protozero reader
 */
inline UnusedAssetFile parse_unused_asset_file(protozero::pbf_reader&& reader) {
  UnusedAssetFile file;

  while (reader.next()) {
    switch (reader.tag()) {
      case 1:
        file.file_name = reader.get_string();
        break;
      case 2:
        file.file_size = reader.get_int64();
        break;
      case 3:
        file.file_md5 = reader.get_string();
        break;
      default:
        reader.skip();
        break;
    }
  }

  return file;
}

/**
 * @brief Parse unused asset info using protozero reader
 */
inline UnusedAssetInfo parse_unused_asset_info(protozero::pbf_reader&& reader) {
  UnusedAssetInfo info;

  while (reader.next()) {
    if (reader.tag() == 1) {  // Assets (repeated)
      info.assets.push_back(parse_unused_asset_file(reader.get_message()));
    } else {
      reader.skip();
    }
  }

  return info;
}

/**
 * @brief Parse unused asset property using protozero reader
 */
inline UnusedAssetProperty parse_unused_asset_property(
    protozero::pbf_reader&& reader) {
  UnusedAssetProperty property;

  while (reader.next()) {
    switch (reader.tag()) {
      case 1:  // Version tag
        property.version_tag = reader.get_string();
        break;
      case 2:  // Asset info (repeated)
        property.asset_infos.push_back(
            parse_unused_asset_info(reader.get_message()));
        break;
      default:
        reader.skip();
        break;
    }
  }

  return property;
}

/**
 * @brief Parse entire patch manifest using callbacks
 * @param data Raw protobuf data
 * @param patch_callback Function called for each patch asset
 * @param unused_callback Function called for each unused asset property
 * @throws std::invalid_argument If data is empty or exceeds maximum size
 */
inline void parse_patch_manifest(
    const std::vector<uint8_t>& data,
    std::function<void(const PatchAsset&)> patch_callback,
    std::function<void(const UnusedAssetProperty&)> unused_callback) {
  if (data.empty()) {
    throw std::invalid_argument("Patch manifest data cannot be empty");
  }
  if (data.size() > kMaxDataSize) {
    throw std::length_error("Patch manifest data size exceeds maximum limit");
  }

  protozero::pbf_reader patch(reinterpret_cast<const char*>(data.data()),
                              data.size());

  while (patch.next()) {
    switch (patch.tag()) {
      case 1:  // Patch assets (repeated)
        if (patch_callback) {
          patch_callback(parse_patch_asset(patch.get_message()));
        } else {
          patch.skip();
        }
        break;
      case 2:  // Unused assets (repeated)
        if (unused_callback) {
          unused_callback(parse_unused_asset_property(patch.get_message()));
        } else {
          patch.skip();
        }
        break;
      default:
        patch.skip();
        break;
    }
  }
}

}  // namespace ProtozeroParser
}  // namespace Quaton

// Compile-time checks
static_assert(std::is_nothrow_move_constructible_v<
                  Quaton::ProtozeroParser::ManifestAssetChunk>,
              "ManifestAssetChunk should be nothrow move constructible");
static_assert(std::is_nothrow_move_constructible_v<
                  Quaton::ProtozeroParser::ManifestAsset>,
              "ManifestAsset should be nothrow move constructible");

#endif  // PROTOZERO_PARSER_HPP
