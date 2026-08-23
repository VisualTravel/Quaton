#ifndef QUATON_MANIFEST_JSON_PARSER_H_
#define QUATON_MANIFEST_JSON_PARSER_H_
#pragma once

#include <map>
#include <string>
#include <type_traits>
#include <vector>

#include "nlohmann/json.hpp"
#include "quaton/manifest/build_api.h"
#include "quaton/manifest/patch_api.h"
#include "quaton/manifest/types.h"
#include "quaton/quaton_global.h"  // For QUATON_API macro

namespace Quaton {

/**
 * @brief Interface for JSON deserialization operations
 */
class JsonDeserializable {
 public:
  virtual ~JsonDeserializable() = default;

 public:
  /// @brief Parse JSON value as string
  /// @param json_value JSON value to parse
  /// @param output Pointer to output string
  /// @return Returns true on success, false on failure
  static bool parse_json_string(const nlohmann::json& json_value,
                                std::string* output);

  /// @brief Parse JSON value as int32
  /// @param json_value JSON value to parse
  /// @param output Pointer to output int32
  /// @return Returns true on success, false on failure
  static bool parse_json_int32(const nlohmann::json& json_value,
                               int32_t* output);

  /// @brief Parse JSON value as int64
  /// @param json_value JSON value to parse
  /// @param output Pointer to output int64
  /// @return Returns true on success, false on failure
  static bool parse_json_int64(const nlohmann::json& json_value,
                               int64_t* output);

  /// @brief Parse JSON value as bool
  /// @param json_value JSON value to parse
  /// @param output Pointer to output bool
  /// @return Returns true on success, false on failure
  static bool parse_json_bool(const nlohmann::json& json_value, bool* output);
};

// ============================================================================
// JSON deserialization function declarations
// ============================================================================

// Build API JSON parsing functions
QUATON_API void parse_from_json(const nlohmann::json& json_data,
                                BuildApiResponse* response);
QUATON_API void parse_from_json(const nlohmann::json& json_data,
                                BuildApiPayload* payload);
QUATON_API void parse_from_json(const nlohmann::json& json_data,
                                ManifestCategoryIdentity* identity);
QUATON_API void parse_from_json(const nlohmann::json& json_data,
                                ManifestFileMetadata* metadata);
QUATON_API void parse_from_json(const nlohmann::json& json_data,
                                ManifestUrlConfiguration* config);
QUATON_API void parse_from_json(const nlohmann::json& json_data,
                                ManifestChunkStatistics* stats);

// Patch API JSON parsing functions
QUATON_API void parse_from_json(const nlohmann::json& json_data,
                                PatchBuildApiResponse* response);
QUATON_API void parse_from_json(const nlohmann::json& json_data,
                                PatchBuildApiPayload* payload);
QUATON_API void parse_from_json(const nlohmann::json& json_data,
                                PatchManifestCategoryIdentity* identity);
QUATON_API void parse_from_json(const nlohmann::json& json_data,
                                PatchOperationStatistics* stats);

// nlohmann/json compatibility convenience overloads
void from_json(const nlohmann::json& json_data, BuildApiResponse& response);
void from_json(const nlohmann::json& json_data, BuildApiPayload& payload);
void from_json(const nlohmann::json& json_data,
               ManifestCategoryIdentity& identity);
void from_json(const nlohmann::json& json_data, ManifestFileMetadata& metadata);
void from_json(const nlohmann::json& json_data,
               ManifestUrlConfiguration& config);
void from_json(const nlohmann::json& json_data, ManifestChunkStatistics& stats);

void from_json(const nlohmann::json& json_data,
               PatchBuildApiResponse& response);
void from_json(const nlohmann::json& json_data, PatchBuildApiPayload& payload);
void from_json(const nlohmann::json& json_data,
               PatchManifestCategoryIdentity& identity);
void from_json(const nlohmann::json& json_data,
               PatchOperationStatistics& stats);

// ============================================================================
// Template utility functions
// ============================================================================

template <typename T, typename Parser>
void parse_json_object(const nlohmann::json& json_data,
                       T* object,
                       Parser&& parser) {
  if (!object) return;

  try {
    parser(json_data, object);
  } catch (const std::exception&) {
    // Re-throw the exception to let the caller handle logging
    throw;
  }
}

/**
 * @brief Safe JSON field parser that automatically handles errors
 * @tparam T Field type
 * @param json_data JSON data
 * @param field_name Field name
 * @param output Output pointer
 * @return Returns true on success, false on failure
 */
template <typename T>
bool safe_parse_json_field(const nlohmann::json& json_data,
                           const std::string& field_name,
                           T* output) {
  try {
    const auto& field = json_data.at(field_name);
    if constexpr (std::is_same_v<T, std::string>) {
      return JsonDeserializable::parse_json_string(field, output);
    } else if constexpr (std::is_same_v<T, int32_t>) {
      return JsonDeserializable::parse_json_int32(field, output);
    } else if constexpr (std::is_same_v<T, int64_t>) {
      return JsonDeserializable::parse_json_int64(field, output);
    } else if constexpr (std::is_same_v<T, bool>) {
      return JsonDeserializable::parse_json_bool(field, output);
    } else {
      static_assert(sizeof(T) == 0,
                    "Unsupported type for safe_parse_json_field");
      return false;
    }
  } catch (const std::exception&) {
    return false;
  }
}

/**
 * @brief Parse JSON array into vector
 * @tparam T Element type
 * @param json_data JSON array data
 * @param output Output vector
 * @param element_parser Element parser function
 */
template <typename T, typename Parser>
void parse_json_array(const nlohmann::json& json_data,
                      std::vector<T>* output,
                      Parser&& element_parser) {
  if (!output) return;

  output->clear();
  // Removed reserve to avoid potential memory waste for variable-sized elements

  for (const auto& element : json_data) {
    T item;
    element_parser(element, &item);
    output->push_back(std::move(item));
  }
}

/**
 * @brief Parse JSON object map
 * @tparam T Value type
 * @param json_data JSON object data
 * @param output Output map
 * @param value_parser Value parser function
 */
template <typename T, typename Parser>
void parse_json_map(const nlohmann::json& json_data,
                    std::map<std::string, T>* output,
                    Parser&& value_parser) {
  if (!output) return;

  output->clear();
  for (auto it = json_data.begin(); it != json_data.end(); ++it) {
    std::string key = it.key();
    T value;
    value_parser(it.value(), &value);
    output->emplace(std::move(key), std::move(value));
  }
}

}  // namespace Quaton

#endif  // QUATON_MANIFEST_JSON_PARSER_H_
