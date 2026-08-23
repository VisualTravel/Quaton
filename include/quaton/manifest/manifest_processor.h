#ifndef QUATON_MANIFEST_PROCESSOR_H_
#define QUATON_MANIFEST_PROCESSOR_H_
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "quaton/data_chunk.h"
#include "quaton/http_client.h"
#include "quaton/manifest/manifest_metadata.h"

namespace Quaton {

// Forward declaration
class Resource;

// ============================================================================
// ManifestProcessor - Manifest processor
// ============================================================================

/**
 * @class ManifestProcessor
 * @brief Quaton manifest processor
 *
 * Provides manifest file download and parsing functionality, supports Zstandard
 * compression
 */
class ManifestProcessor {
 public:
  /**
   * @brief Enumerate/get Quaton resources contained in manifest
   * @param http_client HTTP client instance
   * @param manifest_metadata Manifest information structure
   * @param chunk_info Data chunk information structure
   * @param output_dir Output directory, used to save original and decrypted
   * manifest files (optional)
   * @return Quaton resource list
   * @throws std::invalid_argument If parameters are invalid
   * @throws std::runtime_error If download or parsing fails
   */
  static std::vector<std::shared_ptr<Resource>> enumerate_resources(
      const std::shared_ptr<HttpClient>& http_client,
      const ManifestMetadata& manifest_metadata,
      const Quaton::ChunkInfo& chunk_info,
      const std::string& output_dir = "");

  // Prevent instantiation (static class)
  ManifestProcessor() = delete;
  ManifestProcessor(const ManifestProcessor&) = delete;
  ManifestProcessor& operator=(const ManifestProcessor&) = delete;
};

}  // namespace Quaton

#endif  // QUATON_MANIFEST_PROCESSOR_H_
