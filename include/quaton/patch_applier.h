#ifndef PATCH_APPLIER_H
#define PATCH_APPLIER_H

#pragma once

#include <atomic>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "quaton/manifest/manifest_processor.h"
#include "quaton/manifest/manifest_processor_patch.h"
#include "quaton/progress_callback.h"
#include "quaton/quaton_global.h"  // For QUATON_API macro
#include "quaton/thread_pool.h"

namespace Quaton {

/**
 * @class FileSliceStream
 * @brief File slice stream class
 *
 * Used to read data of specified offset and length from large files.
 */
class QUATON_API FileSliceStream {
 public:
  /**
   * @brief Constructor
   * @param file_path File path
   * @param offset Starting offset
   * @param length Data length
   * @throws std::invalid_argument If file_path is empty or offset/length is
   * invalid
   */
  explicit FileSliceStream(std::string_view file_path,
                           int64_t offset,
                           int64_t length);

  /**
   * @brief Destructor
   */
  ~FileSliceStream() noexcept;

  /**
   * @brief Read data
   * @param buffer Buffer
   * @param size Size to read
   * @return Actual size read
   */
  size_t read(void* buffer, size_t size);

  /**
   * @brief Seek to specified position
   * @param offset Offset
   * @param origin Starting position (SEEK_SET, SEEK_CUR, SEEK_END)
   * @return Whether successful
   */
  bool seek(int64_t offset, int origin);

  /**
   * @brief Get current position
   * @return Current position
   */
  int64_t tell() const noexcept;

  /**
   * @brief Get remaining readable bytes
   * @return Remaining bytes
   */
  int64_t get_remaining_bytes() const noexcept;

  /**
   * @brief Whether reached end of file
   * @return Whether at EOF
   */
  bool is_eof() const noexcept;

 private:
  std::ifstream file_;   ///< File stream
  int64_t offset_;       ///< Starting offset
  int64_t length_;       ///< Data length
  int64_t current_pos_;  ///< Current relative position
};

/**
 * @class PatchApplier
 * @brief Patch applier class
 *
 * Responsible for applying patch files to target files, supports new file
 * creation and old file updates
 */
class PatchApplier {
 public:
  /**
   * @brief Constructor
   * @param output_dir Output directory (game installation directory)
   * @param ldiff_dir Ldiff temporary directory
   * @param thread_count Number of parallel processing threads, 0 means use CPU
   * cores
   * @throws std::invalid_argument If output_dir or ldiff_dir is empty
   */
  explicit PatchApplier(std::string_view output_dir,
                        std::string_view ldiff_dir,
                        int thread_count = 0);

  /**
   * @brief Constructor with package name
   * @param output_dir Output directory (game installation directory)
   * @param ldiff_dir Ldiff temporary directory
   * @param package_name Package name (used to strip prefix from source file
   * paths)
   * @param thread_count Number of parallel processing threads, 0 means use CPU
   * cores
   * @throws std::invalid_argument If output_dir or ldiff_dir is empty
   */
  explicit PatchApplier(std::string_view output_dir,
                        std::string_view ldiff_dir,
                        std::string_view package_name,
                        int thread_count = 0);

  /**
   * @brief Destructor
   */
  ~PatchApplier() noexcept;

  /**
   * @brief Apply all patches
   * @param manifest Patch manifest
   * @return Whether all applications succeeded
   * @throws std::runtime_error If manifest is empty or application fails
   */
  bool apply_patches(const std::shared_ptr<PatchManifestProcessor>& manifest);

  /**
   * @brief Cancel operation
   */
  void cancel() noexcept;

  /**
   * @brief Get list of failed files from last apply operation
   * @return Vector of failed file paths
   */
  std::vector<std::string> get_failed_files() const;

  /**
   * @brief Clear failed files list
   */
  void clear_failed_files();

  /**
   * @brief Set progress callback function
   * @param callback Progress callback function
   */
  void set_progress_callback(ProgressCallback callback);

  /**
   * @brief Delete obsolete files
   * @param manifest Patch manifest
   * @param locked_file_out When non-null and a deletion fails because the
   *        file is locked by another process, set to the offending relative
   *        path and return false so the caller can resolve the lock and retry.
   * @return Whether successful
   * @throws std::runtime_error If manifest is empty
   */
  bool delete_obsolete_files(
      const std::shared_ptr<PatchManifestProcessor>& manifest,
      std::string* locked_file_out = nullptr);

  /**
   * @brief Schedule locked files for deletion at the next user logon.
   *
   * Writes a PowerShell cleanup script under the user data directory and
   * registers it via the HKCU RunOnce key, so files that could not be removed
   * because another process held them open are deleted after a reboot. No-op
   * on non-Windows platforms.
   * @param relative_paths Files (relative to output_dir) to delete at logon
   * @return Whether the task was registered successfully
   */
  bool schedule_runonce_deletion(
      const std::vector<std::string>& relative_paths);

  /**
   * @brief Clean up ldiff temporary directory
   * @return Whether successful
   */
  bool cleanup_ldiff_directory();

 private:
  /**
   * @brief Apply single patch file
   * @param file_info Patch file information
   * @return Whether successful
   */
  bool apply_single_patch(const PatchFileMetadata& file_info);

  /**
   * @brief Create new file (when source file doesn't exist)
   * @param file_info Patch file information
   * @return Whether successful
   */
  bool create_new_file(const PatchFileMetadata& file_info);

  /**
   * @brief Update existing file
   * @param file_info Patch file information
   * @return Whether successful
   */
  bool update_existing_file(const PatchFileMetadata& file_info);

  /**
   * @brief Apply patch using HDiffPatch
   * @param source_file Source file path (nullptr means new file)
   * @param patch_slice Patch data slice
   * @param output_file Output file path
   * @param is_compressed Whether patch is compressed
   * @return Whether successful
   */
  bool apply_hdiff_patch(const std::string* source_file,
                         FileSliceStream& patch_slice,
                         const std::string& output_file,
                         bool is_compressed);

  /**
   * @brief Directly copy patch data (uncompressed case)
   * @param patch_slice Patch data slice
   * @param output_file Output file path
   * @return Whether successful
   */
  bool copy_patch_data(FileSliceStream& patch_slice,
                       const std::string& output_file);

  /**
   * @brief Verify file MD5
   * @param file_path File path
   * @param expected_md5 Expected MD5 hash
   * @return Whether matches
   */
  bool verify_file_md5(std::string_view file_path,
                       std::string_view expected_md5);

  /**
   * @brief Update progress
   * @param current_file Current file
   */
  void update_progress(std::string_view current_file);

  /**
   * @brief Persist files that could not be deleted so a later update can
   * retry them (e.g. a file is momentarily locked on Windows).
   * @param relative_paths Files to retry, relative to the output directory
   */
  void save_pending_deletions(const std::vector<std::string>& relative_paths);

  /**
   * @brief Load the list of files that failed to delete in a previous run.
   * @return Files relative to the output directory
   */
  std::vector<std::string> load_pending_deletions() const;

 private:
  std::string output_dir_;    ///< Output directory
  std::string ldiff_dir_;     ///< Ldiff temporary directory
  std::string package_name_;  ///< Package name, used for path prefix stripping
  int thread_count_;          ///< Number of parallel threads
  std::unique_ptr<ThreadPool>
      thread_pool_;  ///< Thread pool for parallel patch application

  std::atomic<int> completed_files_;  ///< Number of completed files
  std::atomic<bool> cancelled_;       ///< Whether cancelled
  int total_files_;                   ///< Total number of files

  // Progress tracking
  std::unique_ptr<ProgressTracker>
      progress_tracker_;  ///< unified progress tracker

  // Track failed files for summary reporting
  mutable std::mutex failed_files_mutex_;  ///< Mutex for thread-safe access
                                           ///< (mutable for const methods)
  std::vector<std::string> failed_files_;
};

}  // namespace Quaton

#endif  // PATCH_APPLIER_H
