#include "quaton/manifest/json_parser.h"

#include <sstream>
#include <type_traits>
#include <utility>

#include "quaton/logger.h"
#include "quaton/manifest/build_api.h"
#include "quaton/manifest/patch_api.h"
#include "quaton/manifest/types.h"

namespace Quaton {

static std::string parse_category_id(const nlohmann::json& json) {
  auto category_id_json = json.at("category_id");
  if (category_id_json.is_string()) {
    return category_id_json.get<std::string>();
  } else if (category_id_json.is_number()) {
    return std::to_string(category_id_json.get<int>());
  } else {
    throw std::runtime_error("category_id must be string or number");
  }
}

/**
 * @brief Generic API response parsing function
 */
template <typename ResponseT, typename PayloadT>
void parse_api_response(const nlohmann::json& json_data, ResponseT* response) {
  parse_json_object(
      json_data, response, [](const nlohmann::json& json, ResponseT* obj) {
        int32_t status_code;
        std::string status_message;
        PayloadT payload;

        if (!safe_parse_json_field(json, "retcode", &status_code)) {
          throw std::runtime_error("Invalid retcode format");
        }

        if (!safe_parse_json_field(json, "message", &status_message)) {
          throw std::runtime_error("Invalid message format");
        }

        parse_from_json(json.at("data"), &payload);

        obj->set_status_code(status_code);
        obj->set_status_message(std::move(status_message));
        obj->set_response_payload(std::move(payload));
      });
}

/**
 * @brief Generic simple object parsing function (multiple fields)
 */
template <typename T>
void parse_simple_object(
    const nlohmann::json& json_data,
    T* object,
    const std::vector<
        std::pair<std::string, std::function<bool(const nlohmann::json&, T*)>>>&
        fields) {
  parse_json_object(json_data, object, [&](const nlohmann::json& json, T* obj) {
    for (const auto& field : fields) {
      if (!field.second(json, obj)) {
        throw std::runtime_error("Invalid " + field.first + " format");
      }
    }
  });
}

bool ManifestCategoryIdentity::is_complete() const {
  return !category_identifier_.empty() && !matching_pattern_.empty() &&
         manifest_metadata_.is_valid() &&
         !manifest_download_config_.construct_full_url().empty() &&
         !chunk_download_config_.construct_full_url().empty();
}

std::string ManifestCategoryIdentity::get_manifest_download_url() const {
  return manifest_download_config_.construct_full_url();
}

// ============================================================================
// BuildApiPayload implementation
// ============================================================================

const ManifestCategoryIdentity* BuildApiPayload::find_manifest_by_pattern(
    const std::string& pattern) const {
  for (const auto& category : manifest_categories_) {
    if (category.matching_pattern() == pattern) {
      return &category;
    }
  }
  return nullptr;
}

BuildApiResponse::BuildApiResponse(int status_code,
                                   std::string status_message,
                                   BuildApiPayload response_data)
    : status_code_(status_code),
      status_message_(std::move(status_message)),
      response_payload_(std::move(response_data)) {
}

std::string BuildApiResponse::to_debug_string() const {
  std::ostringstream oss;
  oss << "BuildApiResponse{"
      << "code=" << status_code_ << ", "
      << "message='" << status_message_ << "', "
      << "payload=" << (response_payload_.has_manifests() ? "present" : "empty")
      << "}";
  return oss.str();
}

PatchOperationStatistics::PatchOperationStatistics(int64_t compressed_bytes,
                                                   int64_t uncompressed_bytes,
                                                   int file_quantity,
                                                   int chunk_quantity)
    : compressed_size_bytes_(compressed_bytes),
      uncompressed_size_bytes_(uncompressed_bytes),
      file_count_(file_quantity),
      chunk_count_(chunk_quantity) {
}

int64_t PatchOperationStatistics::get_total_size_difference() const {
  return uncompressed_size_bytes_ - compressed_size_bytes_;
}

bool PatchOperationStatistics::is_empty() const {
  return file_count_ == 0 && chunk_count_ == 0 && compressed_size_bytes_ == 0 &&
         uncompressed_size_bytes_ == 0;
}

bool PatchManifestCategoryIdentity::supports_version(
    const std::string& version_tag) const {
  return version_statistics_.find(version_tag) != version_statistics_.end();
}

const PatchOperationStatistics*
PatchManifestCategoryIdentity::get_statistics_for_version(
    const std::string& version) const {
  auto it = version_statistics_.find(version);
  return it != version_statistics_.end() ? &it->second : nullptr;
}

// ============================================================================
// PatchBuildApiPayload implementation
// ============================================================================

int64_t PatchBuildApiPayload::calculate_total_patch_size() const {
  int64_t total_size = 0;
  for (const auto& category : patch_manifest_categories_) {
    for (const auto& stat_pair : category.version_statistics()) {
      total_size += stat_pair.second.compressed_size_bytes();
    }
  }
  return total_size;
}

PatchBuildApiResponse::PatchBuildApiResponse(int status_code,
                                             std::string status_message,
                                             PatchBuildApiPayload response_data)
    : status_code_(status_code),
      status_message_(std::move(status_message)),
      response_payload_(std::move(response_data)) {
}

std::string PatchBuildApiResponse::to_debug_string() const {
  std::ostringstream oss;
  oss << "PatchBuildApiResponse{"
      << "code=" << status_code_ << ", "
      << "message='" << status_message_ << "', "
      << "payload="
      << (response_payload_.has_patch_manifests() ? "present" : "empty") << "}";
  return oss.str();
}

bool JsonDeserializable::parse_json_string(const nlohmann::json& json_value,
                                           std::string* output) {
  if (!json_value.is_string()) {
    return false;
  }
  *output = json_value.get<std::string>();
  return true;
}

bool JsonDeserializable::parse_json_int32(const nlohmann::json& json_value,
                                          int32_t* output) {
  if (json_value.is_number_integer()) {
    *output = json_value.get<int32_t>();
    return true;
  }
  if (json_value.is_string()) {
    try {
      *output = std::stoi(json_value.get<std::string>());
      return true;
    } catch (const std::exception&) {
      return false;
    }
  }
  return false;
}

bool JsonDeserializable::parse_json_int64(const nlohmann::json& json_value,
                                          int64_t* output) {
  if (json_value.is_number_integer()) {
    *output = json_value.get<int64_t>();
    return true;
  }
  if (json_value.is_string()) {
    try {
      *output = std::stoll(json_value.get<std::string>());
      return true;
    } catch (const std::exception&) {
      return false;
    }
  }
  return false;
}

bool JsonDeserializable::parse_json_bool(const nlohmann::json& json_value,
                                         bool* output) {
  if (json_value.is_boolean()) {
    *output = json_value.get<bool>();
    return true;
  }
  if (json_value.is_number_integer()) {
    *output = json_value.get<int>() != 0;
    return true;
  }
  if (json_value.is_string()) {
    const std::string& str = json_value.get<std::string>();
    *output = (str == "true" || str == "1" || str == "yes");
    return true;
  }
  return false;
}

void parse_from_json(const nlohmann::json& json_data,
                     BuildApiResponse* response) {
  parse_api_response<BuildApiResponse, BuildApiPayload>(json_data, response);
}

void parse_from_json(const nlohmann::json& json_data,
                     BuildApiPayload* payload) {
  parse_json_object(
      json_data, payload, [](const nlohmann::json& json, BuildApiPayload* obj) {
        std::string build_id, tag;
        std::vector<ManifestCategoryIdentity> manifests;

        if (!safe_parse_json_field(json, "build_id", &build_id)) {
          throw std::runtime_error("Invalid build_id format");
        }

        if (!safe_parse_json_field(json, "tag", &tag)) {
          throw std::runtime_error("Invalid tag format");
        }

        parse_json_array(
            json.at("manifests"),
            &manifests,
            [](const nlohmann::json& elem, ManifestCategoryIdentity* item) {
              parse_from_json(elem, item);
            });

        obj->set_build_identifier(std::move(build_id));
        obj->set_version_tag(std::move(tag));
        obj->set_manifest_categories(std::move(manifests));
      });
}

void parse_from_json(const nlohmann::json& json_data,
                     ManifestCategoryIdentity* identity) {
  parse_json_object(
      json_data,
      identity,
      [](const nlohmann::json& json, ManifestCategoryIdentity* obj) {
        std::string category_id, category_name, matching_field;
        ManifestFileMetadata manifest_metadata;
        ManifestUrlConfiguration manifest_download, chunk_download;
        ManifestChunkStatistics stats;

        category_id = parse_category_id(json);

        if (!safe_parse_json_field(json, "category_name", &category_name)) {
          throw std::runtime_error("Invalid category_name format");
        }

        if (!safe_parse_json_field(json, "matching_field", &matching_field)) {
          throw std::runtime_error("Invalid matching_field format");
        }

        parse_from_json(json.at("manifest"), &manifest_metadata);
        parse_from_json(json.at("manifest_download"), &manifest_download);
        parse_from_json(json.at("stats"), &stats);
        parse_from_json(json.at("chunk_download"), &chunk_download);

        obj->set_category_identifier(std::move(category_id));
        obj->set_category_display_name(std::move(category_name));
        obj->set_matching_pattern(std::move(matching_field));
        obj->set_manifest_metadata(std::move(manifest_metadata));
        obj->set_manifest_download_config(std::move(manifest_download));
        obj->set_chunk_statistics(std::move(stats));
        obj->set_chunk_download_config(std::move(chunk_download));
      });
}

void parse_from_json(const nlohmann::json& json_data,
                     ManifestFileMetadata* metadata) {
  parse_json_object(
      json_data,
      metadata,
      [](const nlohmann::json& json, ManifestFileMetadata* obj) {
        std::string id, checksum;
        int64_t compressed_size, uncompressed_size;

        if (!safe_parse_json_field(json, "id", &id)) {
          throw std::runtime_error("Invalid manifest id format");
        }

        if (!safe_parse_json_field(json, "checksum", &checksum)) {
          throw std::runtime_error("Invalid checksum format");
        }

        if (!safe_parse_json_field(json, "compressed_size", &compressed_size)) {
          throw std::runtime_error("Invalid compressed_size format");
        }

        if (!safe_parse_json_field(
                json, "uncompressed_size", &uncompressed_size)) {
          throw std::runtime_error("Invalid uncompressed_size format");
        }

        obj->set_identifier(std::move(id));
        obj->set_checksum(std::move(checksum));
        obj->set_compressed_size_bytes(compressed_size);
        obj->set_uncompressed_size_bytes(uncompressed_size);
      });
}

void parse_from_json(const nlohmann::json& json_data,
                     ManifestUrlConfiguration* config) {
  parse_json_object(
      json_data,
      config,
      [](const nlohmann::json& json, ManifestUrlConfiguration* obj) {
        std::string password, url_prefix, url_suffix;
        bool encryption, compression;

        if (!safe_parse_json_field(json, "password", &password)) {
          throw std::runtime_error("Invalid password format");
        }

        if (!safe_parse_json_field(json, "url_prefix", &url_prefix)) {
          throw std::runtime_error("Invalid url_prefix format");
        }

        if (!safe_parse_json_field(json, "url_suffix", &url_suffix)) {
          throw std::runtime_error("Invalid url_suffix format");
        }

        if (!safe_parse_json_field(json, "encryption", &encryption)) {
          throw std::runtime_error("Invalid encryption format");
        }

        if (!safe_parse_json_field(json, "compression", &compression)) {
          throw std::runtime_error("Invalid compression format");
        }

        obj->set_password(std::move(password));
        obj->set_url_prefix(std::move(url_prefix));
        obj->set_url_suffix(std::move(url_suffix));
        obj->set_encrypted(encryption);
        obj->set_compressed(compression);
      });
}

void parse_from_json(const nlohmann::json& json_data,
                     ManifestChunkStatistics* stats) {
  parse_json_object(
      json_data,
      stats,
      [](const nlohmann::json& json, ManifestChunkStatistics* obj) {
        int64_t compressed_size, uncompressed_size;
        int32_t file_count, chunk_count;

        if (!safe_parse_json_field(json, "compressed_size", &compressed_size)) {
          throw std::runtime_error("Invalid compressed_size format");
        }

        if (!safe_parse_json_field(
                json, "uncompressed_size", &uncompressed_size)) {
          throw std::runtime_error("Invalid uncompressed_size format");
        }

        if (!safe_parse_json_field(json, "file_count", &file_count)) {
          throw std::runtime_error("Invalid file_count format");
        }

        if (!safe_parse_json_field(json, "chunk_count", &chunk_count)) {
          throw std::runtime_error("Invalid chunk_count format");
        }

        obj->set_compressed_size_bytes(compressed_size);
        obj->set_uncompressed_size_bytes(uncompressed_size);
        obj->set_file_count(file_count);
        obj->set_chunk_count(chunk_count);
      });
}

void parse_from_json(const nlohmann::json& json_data,
                     PatchBuildApiResponse* response) {
  parse_api_response<PatchBuildApiResponse, PatchBuildApiPayload>(json_data,
                                                                  response);
}

void parse_from_json(const nlohmann::json& json_data,
                     PatchBuildApiPayload* payload) {
  parse_json_object(json_data,
                    payload,
                    [](const nlohmann::json& json, PatchBuildApiPayload* obj) {
                      std::string build_id, patch_id, tag;
                      std::vector<PatchManifestCategoryIdentity> manifests;

                      if (!safe_parse_json_field(json, "build_id", &build_id)) {
                        throw std::runtime_error("Invalid build_id format");
                      }

                      if (!safe_parse_json_field(json, "patch_id", &patch_id)) {
                        throw std::runtime_error("Invalid patch_id format");
                      }

                      if (!safe_parse_json_field(json, "tag", &tag)) {
                        throw std::runtime_error("Invalid tag format");
                      }

                      parse_json_array(json.at("manifests"),
                                       &manifests,
                                       [](const nlohmann::json& elem,
                                          PatchManifestCategoryIdentity* item) {
                                         parse_from_json(elem, item);
                                       });

                      obj->set_build_identifier(std::move(build_id));
                      obj->set_patch_identifier(std::move(patch_id));
                      obj->set_target_version_tag(std::move(tag));
                      obj->set_patch_manifest_categories(std::move(manifests));
                    });
}

void parse_from_json(const nlohmann::json& json_data,
                     PatchManifestCategoryIdentity* identity) {
  parse_json_object(
      json_data,
      identity,
      [](const nlohmann::json& json, PatchManifestCategoryIdentity* obj) {
        std::string category_id, category_name, matching_field;
        ManifestFileMetadata manifest_metadata;
        ManifestUrlConfiguration manifest_download, diff_download;
        std::map<std::string, PatchOperationStatistics> stats_map;

        category_id = parse_category_id(json);

        if (!safe_parse_json_field(json, "category_name", &category_name)) {
          throw std::runtime_error("Invalid category_name format");
        }

        if (!safe_parse_json_field(json, "matching_field", &matching_field)) {
          throw std::runtime_error("Invalid matching_field format");
        }

        parse_from_json(json.at("manifest"), &manifest_metadata);
        parse_from_json(json.at("manifest_download"), &manifest_download);
        parse_from_json(json.at("diff_download"), &diff_download);

        if (json.contains("stats")) {
          parse_json_map(
              json.at("stats"),
              &stats_map,
              [](const nlohmann::json& elem, PatchOperationStatistics* item) {
                parse_from_json(elem, item);
              });
        }

        obj->set_category_identifier(std::move(category_id));
        obj->set_category_display_name(std::move(category_name));
        obj->set_matching_pattern(std::move(matching_field));
        obj->set_manifest_metadata(std::move(manifest_metadata));
        obj->set_manifest_download_config(std::move(manifest_download));
        obj->set_differential_download_config(std::move(diff_download));
        obj->set_version_statistics(std::move(stats_map));
      });
}

void parse_from_json(const nlohmann::json& json_data,
                     PatchOperationStatistics* stats) {
  parse_json_object(
      json_data,
      stats,
      [](const nlohmann::json& json, PatchOperationStatistics* obj) {
        int64_t compressed_size, uncompressed_size;
        int32_t file_count, chunk_count;

        if (!safe_parse_json_field(json, "compressed_size", &compressed_size)) {
          throw std::runtime_error("Invalid compressed_size format");
        }

        if (!safe_parse_json_field(
                json, "uncompressed_size", &uncompressed_size)) {
          throw std::runtime_error("Invalid uncompressed_size format");
        }

        if (!safe_parse_json_field(json, "file_count", &file_count)) {
          throw std::runtime_error("Invalid file_count format");
        }

        if (!safe_parse_json_field(json, "chunk_count", &chunk_count)) {
          throw std::runtime_error("Invalid chunk_count format");
        }

        obj->set_compressed_size_bytes(compressed_size);
        obj->set_uncompressed_size_bytes(uncompressed_size);
        obj->set_file_count(file_count);
        obj->set_chunk_count(chunk_count);
      });
}

void from_json(const nlohmann::json& json_data, BuildApiResponse& response) {
  parse_from_json(json_data, &response);
}

void from_json(const nlohmann::json& json_data, BuildApiPayload& payload) {
  parse_from_json(json_data, &payload);
}

void from_json(const nlohmann::json& json_data,
               ManifestCategoryIdentity& identity) {
  parse_from_json(json_data, &identity);
}

void from_json(const nlohmann::json& json_data,
               ManifestFileMetadata& metadata) {
  parse_from_json(json_data, &metadata);
}

void from_json(const nlohmann::json& json_data,
               ManifestUrlConfiguration& config) {
  parse_from_json(json_data, &config);
}

void from_json(const nlohmann::json& json_data,
               ManifestChunkStatistics& stats) {
  parse_from_json(json_data, &stats);
}

void from_json(const nlohmann::json& json_data,
               PatchBuildApiResponse& response) {
  parse_from_json(json_data, &response);
}

void from_json(const nlohmann::json& json_data, PatchBuildApiPayload& payload) {
  parse_from_json(json_data, &payload);
}

void from_json(const nlohmann::json& json_data,
               PatchManifestCategoryIdentity& identity) {
  parse_from_json(json_data, &identity);
}

void from_json(const nlohmann::json& json_data,
               PatchOperationStatistics& stats) {
  parse_from_json(json_data, &stats);
}

}  // namespace Quaton

// ============================================================================
// Compile-time checks
// ============================================================================

static_assert(
    std::is_nothrow_move_constructible_v<Quaton::ManifestFileMetadata>,
    "ManifestFileMetadata should be nothrow move constructible");
static_assert(
    std::is_nothrow_move_constructible_v<Quaton::ManifestUrlConfiguration>,
    "ManifestUrlConfiguration should be nothrow move constructible");
static_assert(
    std::is_nothrow_move_constructible_v<Quaton::ManifestChunkStatistics>,
    "ManifestChunkStatistics should be nothrow move constructible");
static_assert(
    std::is_nothrow_move_constructible_v<Quaton::ManifestCategoryIdentity>,
    "ManifestCategoryIdentity should be nothrow move constructible");
static_assert(std::is_nothrow_move_constructible_v<Quaton::BuildApiPayload>,
              "BuildApiPayload should be nothrow move constructible");
static_assert(std::is_nothrow_move_constructible_v<Quaton::BuildApiResponse>,
              "BuildApiResponse should be nothrow move constructible");
static_assert(
    std::is_nothrow_move_constructible_v<Quaton::PatchOperationStatistics>,
    "PatchOperationStatistics should be nothrow move constructible");
static_assert(
    std::is_nothrow_move_constructible_v<Quaton::PatchManifestCategoryIdentity>,
    "PatchManifestCategoryIdentity should be nothrow move constructible");
static_assert(
    std::is_nothrow_move_constructible_v<Quaton::PatchBuildApiPayload>,
    "PatchBuildApiPayload should be nothrow move constructible");
static_assert(
    std::is_nothrow_move_constructible_v<Quaton::PatchBuildApiResponse>,
    "PatchBuildApiResponse should be nothrow move constructible");
