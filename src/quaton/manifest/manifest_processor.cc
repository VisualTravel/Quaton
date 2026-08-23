#include "quaton/manifest/manifest_processor.h"

#include <zstd.h>

#include <algorithm>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "quaton/configuration/configuration.h"
#include "quaton/data_chunk.h"
#include "quaton/logger.h"
#include "quaton/manifest/json_parser.h"
#include "quaton/protozero_parser.hpp"
#include "quaton/resource.h"
#include "quaton/url.h"
#include "quaton/utils/compression.h"

namespace Quaton {

std::vector<std::shared_ptr<Resource>> ManifestProcessor::enumerate_resources(
    const std::shared_ptr<HttpClient>& http_client,
    const ManifestMetadata& manifest_metadata,
    const Quaton::ChunkInfo& chunk_info,
    const std::string& output_dir) {
  if (!http_client) {
    throw std::invalid_argument("http_client cannot be null");
  }
  if (!manifest_metadata.is_valid()) {
    throw std::invalid_argument("Invalid manifest metadata provided");
  }
  if (chunk_info.get_base_url().empty()) {
    throw std::invalid_argument("chunk_info.base_url cannot be empty");
  }

  std::vector<std::shared_ptr<Resource>> resources;

  try {
    const std::string file_url = manifest_metadata.get_file_url();
    LOG_INFO("Downloading manifest from: %s", file_url.c_str());

    auto manifest_data_future = http_client->get_async(file_url);
    std::vector<uint8_t> raw_data = manifest_data_future.get();

    if (raw_data.empty()) {
      throw std::runtime_error("Failed to download manifest: empty response");
    }

    LOG_INFO("Downloaded manifest, size: %zu bytes", raw_data.size());

    std::vector<uint8_t> manifest_data;
    if (manifest_metadata.get_use_compression()) {
      manifest_data = CompressionUtils::DecompressZstd(raw_data);
    } else {
      manifest_data = std::move(raw_data);
    }

    Quaton::ProtozeroParser::parse_manifest(
        manifest_data,
        [&](const Quaton::ProtozeroParser::ManifestAsset& parsed_asset) {
          // LOG_DEBUG("Parsed asset: %s, size: %lld, type: %d, chunks: %zu",
          //          parsed_asset.asset_name.c_str(), parsed_asset.asset_size,
          //          parsed_asset.asset_type,
          //          parsed_asset.asset_chunks.size());

          auto resource = std::make_shared<Resource>();
          resource->name_ = parsed_asset.asset_name;
          resource->size_ = parsed_asset.asset_size;
          resource->hash_ = parsed_asset.asset_hash_md5;
          resource->is_directory_ = (parsed_asset.asset_type == 1);
          resource->has_patch_ = false;
          resource->chunks_info_ =
              std::make_shared<Quaton::ChunkInfo>(chunk_info);

          resource->chunks_.reserve(parsed_asset.asset_chunks.size());
          for (const auto& chunk_property : parsed_asset.asset_chunks) {
            Quaton::DataChunk chunk;
            chunk.name_ = chunk_property.chunk_name;
            chunk.offset_ = chunk_property.asset_offset;
            chunk.container_offset_ = chunk_property.chunk_on_file_offset;
            chunk.size_ = chunk_property.chunk_size;
            chunk.decompressed_size_ = chunk_property.chunk_size_decompressed;
            chunk.decompressed_hash_ = std::vector<uint8_t>(
                chunk_property.chunk_decompressed_hash_md5.begin(),
                chunk_property.chunk_decompressed_hash_md5.end());
            resource->chunks_.push_back(std::move(chunk));
          }

          resources.push_back(std::move(resource));
        });

    LOG_INFO("Manifest parsing completed. Total assets parsed: %zu",
             resources.size());

  } catch (const std::exception& e) {
    LOG_ERROR("Error in ManifestProcessor::enumerate_resources: %s", e.what());
    throw;
  }

  return resources;
}

}  // namespace Quaton
