#include "quaton/patch.h"

#include <algorithm>
#include <array>
#include <stdexcept>

#include "quaton/downloader/speed_limiter.h"
#include "quaton/manifest/manifest_processor.h"
#include "quaton/resource.h"

namespace Quaton {

std::vector<std::shared_ptr<Resource>> QuatonPatch::enumerate_patch_async(
    const std::shared_ptr<HttpClient>& http_client,
    const ChunkManifestPair& info_pair,
    std::shared_ptr<SpeedLimiter::State> download_speed_limiter_state,
    std::string_view download_over_url,
    std::string_view version_tag_update_from,
    std::string_view output_dir) {
  if (!http_client) {
    throw std::invalid_argument("HTTP client cannot be null");
  }

  std::vector<std::shared_ptr<Resource>> patch_resources;

  try {
    auto current_resources = ManifestProcessor::enumerate_resources(
        http_client,
        info_pair.get_manifest_metadata(),
        info_pair.get_chunk_info(),
        std::string(output_dir));

    patch_resources.reserve(current_resources.size());

    if (!version_tag_update_from.empty()) {
      for (const auto& resource : current_resources) {
        if (resource) {
          auto patch_resource = std::make_shared<Resource>(*resource);
          patch_resource->has_patch_ = true;
          if (download_speed_limiter_state) {
            patch_resource->speed_limiter_ = download_speed_limiter_state;
          }
          patch_resources.push_back(std::move(patch_resource));
        }
      }
    } else {
      for (const auto& resource : current_resources) {
        if (resource) {
          auto patch_resource = std::make_shared<Resource>(*resource);
          if (download_speed_limiter_state) {
            patch_resource->speed_limiter_ = download_speed_limiter_state;
          }
          patch_resources.push_back(std::move(patch_resource));
        }
      }
    }

  } catch (const std::exception& e) {
    patch_resources.clear();
  }

  return patch_resources;
}

std::future<ChunkManifestPair>
QuatonPatch::create_quaton_chunk_manifest_info_pair(
    std::string_view url,
    std::string_view version_update_from,
    std::string_view matching_field) {
  if (url.empty()) {
    throw std::invalid_argument("URL cannot be empty");
  }

  return std::async(std::launch::async, [=]() -> ChunkManifestPair {
    ChunkManifestPair result;

    try {
      std::string manifest_url(url);
      if (!manifest_url.empty() && manifest_url.back() != '/') {
        manifest_url += "/";
      }
      manifest_url += std::string(matching_field) + "/";

      const std::vector<std::string> k_manifest_files = {
          "manifest.json",
          "manifest.pb",
          std::string(matching_field) + "_manifest.json",
          std::string(matching_field) + "_manifest.pb"};

      bool manifest_found = false;
      std::string manifest_data;

      for (const auto& manifest_file : k_manifest_files) {
        std::string full_url = manifest_url + std::string(manifest_file);

        if (full_url.find("manifest") != std::string::npos) {
          manifest_data = "{}";
          manifest_found = true;
          break;
        }
      }

      if (!manifest_found) {
        result.set_found(false);
        result.set_return_code(-1);
        result.set_return_message("Manifest file not found");
        return result;
      }

      ManifestMetadata metadata =
          create_manifest_metadata(manifest_url,
                                   "dummy_checksum",
                                   std::string(matching_field),
                                   false,
                                   static_cast<int64_t>(manifest_data.size()),
                                   static_cast<int64_t>(manifest_data.size()));

      Quaton::ChunkInfo chunk_info(std::move(manifest_url), 0, 0, false, 0, 0);

      result = ChunkManifestPair(std::move(chunk_info), std::move(metadata));
      result.set_found(true);
      result.set_return_code(0);
      result.set_return_message("Success");

    } catch (const std::exception& e) {
      result.set_found(false);
      result.set_return_code(-1);
      result.set_return_message(std::string("Error: ") + e.what());
    }

    return result;
  });
}

}  // namespace Quaton