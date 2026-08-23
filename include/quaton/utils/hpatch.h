#ifndef UTILS_HPATCH_H
#define UTILS_HPATCH_H
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "quaton/quaton_global.h"

QUATON_NAMESPACE_BEGIN
namespace HPatchUtils {

/**
 * @struct HPatchResult
 * @brief Result of patch application operation
 */
struct HPatchResult {
  bool success;               ///< Whether operation succeeded
  int exit_code;              ///< Process exit code
  std::string error_message;  ///< Error message if failed

  HPatchResult() : success(false), exit_code(-1) {}
};

/**
 * @brief Get hpatchz executable path
 * @return Path to the hpatchz executable
 *
 * Patching runs in-process through the vendored HDiffPatch library; no
 * external hpatchz executable is required. Kept for API compatibility.
 */
QUATON_API std::string get_hpatchz_path();

/**
 * @brief Apply HDiff patch to create output file
 * @param source_file Source file path (nullptr or empty for new file creation)
 * @param patch_file Patch file path
 * @param output_file Output file path
 * @param is_compressed Reserved; compression is auto-detected from the head
 * @return Patch application result
 */
QUATON_API HPatchResult apply_patch(const std::string* source_file,
                                    const std::string& patch_file,
                                    const std::string& output_file,
                                    bool is_compressed = false);

/**
 * @brief Apply HDiff patch using in-memory patch data
 * @param source_file Source file path (nullptr or empty for new file creation)
 * @param patch_data Patch data in memory
 * @param patch_size Size of patch data
 * @param output_file Output file path
 * @param is_compressed Reserved; compression is auto-detected from the head
 * @return Patch application result
 *
 * The patch is applied directly from memory; no temporary file is created.
 */
QUATON_API HPatchResult apply_patch_from_memory(const std::string* source_file,
                                                const uint8_t* patch_data,
                                                size_t patch_size,
                                                const std::string& output_file,
                                                bool is_compressed = false);

/**
 * @brief Verify if patch support is available
 * @return true (patching is always available in-process)
 */
QUATON_API bool is_hpatchz_available();

/**
 * @brief Get the bundled HDiffPatch library version
 * @return Version string
 */
QUATON_API std::string get_hpatchz_version();

}  // namespace HPatchUtils
QUATON_NAMESPACE_END

#endif  // UTILS_HPATCH_H
