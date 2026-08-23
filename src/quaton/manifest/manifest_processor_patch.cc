#include "quaton/manifest/manifest_processor_patch.h"

#include <fstream>
#include <stdexcept>

#include "quaton/logger.h"
#include "quaton/protozero_parser.hpp"
#include "quaton/utils/checksum.h"
#include "quaton/utils/compression.h"
#include "quaton/utils/path_helper.h"

namespace Quaton {

/**
 * @brief Decompress manifest data using Zstandard
 * @param compressed_data Compressed data
 * @param compressed Whether data is compressed
 * @param output_dir Output directory (optional)
 * @return Decompressed data
 */
static std::vector<uint8_t> decompress_manifest_data(
    const std::vector<uint8_t>& compressed_data,
    bool compressed,
    const std::string& output_dir) {
  if (!compressed) {
    return compressed_data;
  }

  std::vector<uint8_t> decompressed_data =
      CompressionUtils::DecompressZstd(compressed_data, output_dir);

  LOG_INFO("Decompressed manifest data in memory, size: %zu bytes",
           decompressed_data.size());

  return decompressed_data;
}

std::shared_ptr<PatchManifestProcessor>
PatchManifestProcessor::download_and_parse_manifest(
    const std::shared_ptr<HttpClient>& http_client,
    const std::string& manifest_url,
    bool compressed,
    const std::string& expected_md5,
    const std::string& source_version,
    const std::string& manifest_identifier,
    const std::string& output_dir) {
  if (!http_client) {
    throw std::invalid_argument("http_client cannot be null");
  }
  if (manifest_url.empty()) {
    throw std::invalid_argument("manifest_url cannot be empty");
  }
  if (source_version.empty()) {
    throw std::invalid_argument("source_version cannot be empty");
  }

  try {
    LOG_INFO("Downloading patch manifest from: %s", manifest_url.c_str());

    auto response_data = http_client->get_async(manifest_url).get();

    if (response_data.empty()) {
      throw std::runtime_error(
          "Failed to download patch manifest: empty response");
    }

    LOG_INFO("Downloaded patch manifest, size: %zu bytes",
             response_data.size());

    if (!output_dir.empty() && !manifest_identifier.empty()) {
      std::string output_path = output_dir + "/" + manifest_identifier;
      PathHelper::CreateDirectoryRecursive(output_dir);

      std::ofstream out_file(output_path, std::ios::binary);
      if (out_file.is_open()) {
        out_file.write(reinterpret_cast<const char*>(response_data.data()),
                       response_data.size());
        out_file.close();
        LOG_INFO("Saved compressed patch manifest to: %s", output_path.c_str());
      } else {
        LOG_WARN("Failed to save compressed patch manifest to: %s",
                 output_path.c_str());
      }
    }

    std::vector<uint8_t> decompressed_data =
        decompress_manifest_data(response_data, compressed, "");

    if (!expected_md5.empty()) {
      std::string actual_md5 = ChecksumUtils::calculate_md5_data(
          decompressed_data.data(), decompressed_data.size());
      if (actual_md5 != expected_md5) {
        throw std::runtime_error("Patch manifest MD5 mismatch! Expected: " +
                                 expected_md5 + ", Actual: " + actual_md5);
      }
      LOG_INFO("Patch manifest MD5 verified: %s", actual_md5.c_str());
    }

    return parse_manifest_from_data(decompressed_data, source_version);

  } catch (const std::exception& e) {
    LOG_ERROR(
        "Exception in PatchManifestProcessor::download_and_parse_manifest: %s",
        e.what());
    throw;
  }
}

std::shared_ptr<PatchManifestProcessor>
PatchManifestProcessor::parse_manifest_from_file(
    const std::string& manifest_path,
    bool compressed,
    const std::string& source_version) {
  if (manifest_path.empty()) {
    throw std::invalid_argument("manifest_path cannot be empty");
  }
  if (source_version.empty()) {
    throw std::invalid_argument("source_version cannot be empty");
  }

  try {
    LOG_INFO("Parsing patch manifest from file: %s", manifest_path.c_str());

    std::ifstream in_file(manifest_path, std::ios::binary | std::ios::ate);
    if (!in_file.is_open()) {
      throw std::runtime_error("Failed to open patch manifest file: " +
                               manifest_path);
    }

    std::streamsize file_size = in_file.tellg();
    if (file_size <= 0) {
      throw std::runtime_error("Patch manifest file is empty or invalid: " +
                               manifest_path);
    }

    in_file.seekg(0, std::ios::beg);

    std::vector<uint8_t> file_data(static_cast<size_t>(file_size));
    in_file.read(reinterpret_cast<char*>(file_data.data()), file_size);
    in_file.close();

    LOG_INFO("Read patch manifest file, size: %zu bytes", file_data.size());

    std::vector<uint8_t> decompressed_data =
        decompress_manifest_data(file_data, compressed, "");

    return parse_manifest_from_data(decompressed_data, source_version);

  } catch (const std::exception& e) {
    LOG_ERROR(
        "Exception in PatchManifestProcessor::parse_manifest_from_file: %s",
        e.what());
    throw;
  }
}

std::shared_ptr<PatchManifestProcessor>
PatchManifestProcessor::parse_manifest_from_data(
    const std::vector<uint8_t>& data, const std::string& source_version) {
  if (source_version.empty()) {
    throw std::invalid_argument("source_version cannot be empty");
  }

  try {
    // An empty payload is a valid publish with zero patches: the new version
    // contains no changes for this package, so no patch files exist. Treat it
    // as an empty manifest instead of failing the whole update.
    if (data.empty()) {
      LOG_INFO("Patch manifest is empty (no changes in this publish)");
      return std::make_shared<PatchManifestProcessor>(source_version);
    }

    LOG_INFO("Parsing patch manifest protobuf data, size: %zu bytes",
             data.size());

    auto processor = std::make_shared<PatchManifestProcessor>(source_version);

    LOG_INFO("Filtering patches for source version: %s",
             source_version.c_str());

    int matched_count = 0;
    int skipped_count = 0;

    Quaton::ProtozeroParser::parse_patch_manifest(
        data,
        [&](const Quaton::ProtozeroParser::PatchAsset& patch_asset) {
          bool found_match = false;
          for (const auto& asset_info : patch_asset.asset_infos) {
            if (asset_info.version_tag == source_version) {
              found_match = true;
              PatchFileMetadata patch_file;

              patch_file.target_path_ = patch_asset.asset_name;
              patch_file.target_size_ = patch_asset.asset_size;
              patch_file.target_md5_ = patch_asset.asset_hash_md5;
              patch_file.source_version_tag_ = asset_info.version_tag;

              const auto& chunk = asset_info.chunk;
              patch_file.patch_identifier_ = chunk.patch_name;
              patch_file.patch_total_size_ = chunk.patch_size;
              patch_file.patch_md5_ = chunk.patch_md5;
              patch_file.patch_data_offset_ = chunk.patch_offset;
              patch_file.patch_data_length_ = chunk.patch_length;
              patch_file.source_file_name_ = chunk.original_file_name;
              patch_file.source_file_size_ = chunk.original_file_length;
              patch_file.source_file_md5_ = chunk.original_file_md5;

              patch_file.is_new_file_ = chunk.original_file_name.empty();
              patch_file.is_compressed_ =
                  false;  // Default, will be set by manifest_mapping

              if (patch_file.is_valid()) {
                processor->add_patch_file(std::move(patch_file));
                matched_count++;

                processor->patch_detail_map_[chunk.patch_name] =
                    std::make_pair(chunk.patch_size, chunk.patch_md5);
              } else {
                LOG_WARN("Invalid patch file metadata for: %s",
                         patch_asset.asset_name.c_str());
              }

              break;
            }
          }

          // Log if no matching version found
          if (!found_match && !patch_asset.asset_infos.empty()) {
            skipped_count++;
            if (skipped_count <= 3) {  // Only log first 3 to avoid spam
              LOG_DEBUG(
                  "Asset '%s' has no patch for version %s (available: %s)",
                  patch_asset.asset_name.c_str(),
                  source_version.c_str(),
                  patch_asset.asset_infos[0].version_tag.c_str());
            }
          }
        },
        [&](const Quaton::ProtozeroParser::UnusedAssetProperty&
                unused_property) {
          for (const auto& asset_info : unused_property.asset_infos) {
            for (const auto& unused_asset : asset_info.assets) {
              ObsoleteFileMetadata obsolete_file;

              obsolete_file.file_path_ = unused_asset.file_name;
              obsolete_file.file_size_ = unused_asset.file_size;
              obsolete_file.file_md5_ = unused_asset.file_md5;

              if (obsolete_file.is_valid()) {
                processor->add_obsolete_file(std::move(obsolete_file));
              } else {
                LOG_WARN("Invalid obsolete file metadata for: %s",
                         unused_asset.file_name.c_str());
              }
            }
          }
        });

    LOG_INFO(
        "Patch manifest parsing completed. Patch files: %zu, Obsolete files: "
        "%zu",
        processor->get_patch_file_count(),
        processor->get_obsolete_file_count());
    LOG_INFO("Version filter stats: %d matched, %d skipped (different version)",
             matched_count,
             skipped_count);

    return processor;

  } catch (const std::exception& e) {
    LOG_ERROR(
        "Exception in PatchManifestProcessor::parse_manifest_from_data: %s",
        e.what());
    throw;  // Re-throw exception to preserve exception safety
  }
}

std::vector<std::string>
PatchManifestProcessor::get_distinct_patch_identifiers() const {
  std::vector<std::string> patch_identifiers;
  const auto& detail_map = get_patch_detail_map();
  patch_identifiers.reserve(detail_map.size());

  for (const auto& pair : detail_map) {
    patch_identifiers.push_back(pair.first);
  }

  return patch_identifiers;
}

bool PatchManifestProcessor::retrieve_patch_details(
    const std::string& patch_id,
    int64_t& out_size,
    std::string& out_md5) const {
  if (patch_id.empty()) {
    return false;
  }

  const auto& detail_map = get_patch_detail_map();
  auto it = detail_map.find(patch_id);
  if (it != detail_map.end()) {
    out_size = it->second.first;
    out_md5 = it->second.second;
    return true;
  }
  return false;
}

int64_t PatchManifestProcessor::calculate_total_patch_size() const {
  int64_t total_size = 0;
  const auto& detail_map = get_patch_detail_map();
  for (const auto& pair : detail_map) {
    total_size += pair.second.first;
  }
  return total_size;
}

bool PatchFileMetadata::is_valid() const noexcept {
  return !target_path_.empty() && !target_md5_.empty() && target_size_ > 0 &&
         !source_version_tag_.empty() && !patch_identifier_.empty() &&
         patch_total_size_ > 0 && !patch_md5_.empty() &&
         patch_data_length_ > 0 &&
         (is_new_file_ || (!source_file_name_.empty() &&
                           source_file_size_ > 0 && !source_file_md5_.empty()));
}

bool ObsoleteFileMetadata::is_valid() const noexcept {
  return !file_path_.empty() && file_size_ > 0 && !file_md5_.empty();
}

}  // namespace Quaton
