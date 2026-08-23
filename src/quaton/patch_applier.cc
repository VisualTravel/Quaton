#include "quaton/patch_applier.h"

#include <zstd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <thread>

#include "quaton/downloader/downloader_utils.h"
#include "quaton/logger.h"
#include "quaton/manifest/manifest_processor_patch.h"
#include "quaton/utils/checksum.h"
#include "quaton/utils/hpatch.h"
#include "quaton/utils/path_helper.h"

#ifdef QUATON_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace Quaton {

namespace {

// Outcome of one deletion attempt: removed, failed for a transient reason
// (defer to next run), or locked by another process (report to the caller).
enum class DeleteResult { kDeleted, kGenericFail, kLocked };

// Windows marks a file that another process has open with
// ERROR_SHARING_VIOLATION (32) or ERROR_LOCK_VIOLATION (33); treat those as a
// user-resolvable lock.
bool is_lock_error(const std::error_code& ec) {
#ifdef QUATON_PLATFORM_WINDOWS
  return ec.value() == 32 || ec.value() == 33;
#else
  return false;
#endif
}

// Doubles single quotes so a path is safe inside a single-quoted PS string.
std::string escape_single_quote(const std::string& path) {
  std::string out;
  out.reserve(path.size());
  for (const char c : path) {
    if (c == '\'') {
      out += "''";
    } else {
      out += c;
    }
  }
  return out;
}

}  // namespace

FileSliceStream::FileSliceStream(std::string_view file_path,
                                 int64_t offset,
                                 int64_t length)
    : offset_(offset), length_(length), current_pos_(0) {
  if (file_path.empty()) {
    throw std::invalid_argument("File path cannot be empty");
  }
  if (offset < 0 || length < 0) {
    throw std::invalid_argument("Invalid offset or length");
  }

  file_.open(std::string(file_path), std::ios::binary);
  if (!file_.is_open()) {
    throw std::runtime_error("Failed to open file: " + std::string(file_path));
  }

  file_.seekg(offset_, std::ios::beg);
  if (file_.fail()) {
    throw std::runtime_error("Failed to seek to offset: " +
                             std::to_string(offset));
  }
}

FileSliceStream::~FileSliceStream() noexcept {
  if (file_.is_open()) {
    file_.close();
  }
}

size_t FileSliceStream::read(void* buffer, size_t size) {
  if (!file_.is_open() || is_eof()) {
    return 0;
  }

  int64_t remaining = get_remaining_bytes();
  size_t to_read = (std::min)(static_cast<int64_t>(size), remaining);

  file_.read(static_cast<char*>(buffer), to_read);
  size_t actual_read = file_.gcount();

  current_pos_ += actual_read;
  return actual_read;
}

bool FileSliceStream::seek(int64_t offset, int origin) {
  int64_t new_pos = 0;

  switch (origin) {
    case SEEK_SET:
      new_pos = offset;
      break;
    case SEEK_CUR:
      new_pos = current_pos_ + offset;
      break;
    case SEEK_END:
      new_pos = length_ + offset;
      break;
    default:
      return false;
  }

  if (new_pos < 0 || new_pos > length_) {
    return false;
  }

  file_.seekg(offset_ + new_pos, std::ios::beg);
  if (file_.fail()) {
    return false;
  }

  current_pos_ = new_pos;
  return true;
}

int64_t FileSliceStream::tell() const noexcept {
  return current_pos_;
}

int64_t FileSliceStream::get_remaining_bytes() const noexcept {
  return length_ - current_pos_;
}

bool FileSliceStream::is_eof() const noexcept {
  return current_pos_ >= length_;
}

PatchApplier::PatchApplier(std::string_view output_dir,
                           std::string_view ldiff_dir,
                           int thread_count)
    : output_dir_(output_dir),
      ldiff_dir_(ldiff_dir),
      package_name_(""),
      thread_count_(thread_count),
      completed_files_(0),
      cancelled_(false),
      total_files_(0),
      progress_tracker_(nullptr) {
  if (output_dir_.empty()) {
    throw std::invalid_argument("Output directory cannot be empty");
  }
  if (ldiff_dir_.empty()) {
    throw std::invalid_argument("Ldiff directory cannot be empty");
  }

  if (thread_count_ <= 0) {
    thread_count_ = (std::max)(1u, std::thread::hardware_concurrency());
  }

  thread_pool_ = std::make_unique<ThreadPool>(thread_count_);
  LOG_INFO("PatchApplier initialized with %d threads", thread_count_);
}

PatchApplier::PatchApplier(std::string_view output_dir,
                           std::string_view ldiff_dir,
                           std::string_view package_name,
                           int thread_count)
    : output_dir_(output_dir),
      ldiff_dir_(ldiff_dir),
      package_name_(package_name),
      thread_count_(thread_count),
      completed_files_(0),
      cancelled_(false),
      total_files_(0),
      progress_tracker_(nullptr) {
  if (output_dir_.empty()) {
    throw std::invalid_argument("Output directory cannot be empty");
  }
  if (ldiff_dir_.empty()) {
    throw std::invalid_argument("Ldiff directory cannot be empty");
  }

  if (thread_count_ <= 0) {
    thread_count_ = (std::max)(1u, std::thread::hardware_concurrency());
  }

  thread_pool_ = std::make_unique<ThreadPool>(thread_count_);
  LOG_INFO("PatchApplier initialized with package '%s' and %d threads",
           package_name_.c_str(),
           thread_count_);
}

PatchApplier::~PatchApplier() noexcept {
  cancel();
}

bool PatchApplier::apply_patches(
    const std::shared_ptr<PatchManifestProcessor>& manifest) {
  if (!manifest) {
    LOG_ERROR("Manifest is null");
    throw std::invalid_argument("Manifest cannot be null");
  }

  const auto& patch_files = manifest->get_patch_file_list();
  total_files_ = static_cast<int>(patch_files.size());
  completed_files_ = 0;
  cancelled_ = false;

  // Clear previous failed files list
  clear_failed_files();

  // Initialize progress tracker
  progress_tracker_ = std::make_unique<ProgressTracker>(
      total_files_, OperationMode::kPatchLocalInstall);

  LOG_INFO("Starting to apply %d patches", total_files_);

  std::vector<std::future<bool>> futures;
  futures.reserve(patch_files.size());

  for (const auto& file_info : patch_files) {
    auto future = thread_pool_->enqueue([this, file_info]() -> bool {
      if (cancelled_) {
        return false;
      }

      // Update progress tracker
      if (progress_tracker_) {
        progress_tracker_->update_current_file(file_info.target_path_,
                                               FileProcessStage::kDecompressed);
      }

      LOG_INFO("Applying patch for: %s", file_info.target_path_.c_str());

      if (!apply_single_patch(file_info)) {
        LOG_ERROR("Failed to apply patch for: %s",
                  file_info.target_path_.c_str());
        return false;
      }

      completed_files_++;

      // Update progress tracker on completion
      if (progress_tracker_) {
        progress_tracker_->update_current_file(file_info.target_path_,
                                               FileProcessStage::kApplied);
        progress_tracker_->increment_completed();
      }

      LOG_INFO("Completed patch for: %s (%d/%d)",
               file_info.target_path_.c_str(),
               completed_files_.load(),
               total_files_);
      return true;
    });
    futures.push_back(std::move(future));
  }

  bool has_error = false;
  for (auto& future : futures) {
    try {
      if (!future.get()) {
        has_error = true;
      }
    } catch (const std::exception& e) {
      LOG_ERROR("Exception in patch application: %s", e.what());
      has_error = true;
    }
  }

  if (cancelled_) {
    LOG_INFO("Patch application cancelled");
    return false;
  }

  if (has_error) {
    std::lock_guard<std::mutex> lock(failed_files_mutex_);
    LOG_ERROR("Patch application completed with %zu file(s) failed",
              failed_files_.size());
    if (!failed_files_.empty()) {
      LOG_ERROR("Failed files list:");
      for (const auto& failed_file : failed_files_) {
        LOG_ERROR("  - %s", failed_file.c_str());
      }
    }
    throw std::runtime_error(
        "Patch application failed: " + std::to_string(failed_files_.size()) +
        " file(s) failed");
  }

  LOG_INFO("All patches applied successfully");
  return true;
}

void PatchApplier::cancel() noexcept {
  cancelled_ = true;
}

std::vector<std::string> PatchApplier::get_failed_files() const {
  std::lock_guard<std::mutex> lock(failed_files_mutex_);
  return failed_files_;
}

void PatchApplier::clear_failed_files() {
  std::lock_guard<std::mutex> lock(failed_files_mutex_);
  failed_files_.clear();
}

void PatchApplier::set_progress_callback(ProgressCallback callback) {
  if (progress_tracker_) {
    progress_tracker_->set_callback(std::move(callback));
  }
}

bool PatchApplier::apply_single_patch(const PatchFileMetadata& file_info) {
  try {
    namespace fs = std::filesystem;

    fs::path target_file_path = fs::path(output_dir_) / file_info.target_path_;

    if (target_file_path.has_parent_path()) {
      fs::create_directories(target_file_path.parent_path());
    }

    bool success = false;
    if (file_info.is_new_file_) {
      LOG_INFO("Creating new file: %s (version: %s)",
               file_info.target_path_.c_str(),
               file_info.source_version_tag_.c_str());
      success = create_new_file(file_info);
    } else {
      LOG_INFO("Updating existing file: %s (from version: %s)",
               file_info.target_path_.c_str(),
               file_info.source_version_tag_.c_str());
      success = update_existing_file(file_info);
    }

    if (!success) {
      std::lock_guard<std::mutex> lock(failed_files_mutex_);
      failed_files_.push_back(file_info.target_path_);
      return false;
    }

    if (!file_info.target_md5_.empty()) {
      std::string target_path_str = target_file_path.string();

      if (!verify_file_md5(target_path_str, file_info.target_md5_)) {
        LOG_ERROR("Target file MD5 verification failed: %s",
                  file_info.target_path_.c_str());
        LOG_ERROR("  Source version: %s",
                  file_info.source_version_tag_.c_str());
        LOG_ERROR("  Patch file: %s", file_info.patch_identifier_.c_str());
        if (!file_info.source_file_md5_.empty()) {
          LOG_ERROR("  Expected source MD5: %s",
                    file_info.source_file_md5_.c_str());
        }
        std::lock_guard<std::mutex> lock(failed_files_mutex_);
        failed_files_.push_back(file_info.target_path_);
        return false;
      }

      std::string calculated_xxhash =
          ChecksumUtils::calculate_xxhash_file(target_path_str);
      if (calculated_xxhash.empty()) {
        LOG_ERROR("Failed to calculate XXHash for target file: %s",
                  file_info.target_path_.c_str());
        std::lock_guard<std::mutex> lock(failed_files_mutex_);
        failed_files_.push_back(file_info.target_path_);
        return false;
      }

      LOG_INFO("Target file verified and XXHash calculated: %s",
               file_info.target_path_.c_str());
    }

    return true;

  } catch (const std::exception& e) {
    LOG_ERROR("Exception in apply_single_patch: %s", e.what());
    std::lock_guard<std::mutex> lock(failed_files_mutex_);
    failed_files_.push_back(file_info.target_path_);
    return false;
  }
}

bool PatchApplier::create_new_file(const PatchFileMetadata& file_info) {
  try {
    namespace fs = std::filesystem;
    fs::path patch_path = fs::path(ldiff_dir_) / file_info.patch_identifier_;

    LOG_DEBUG(
        "Creating new file: patch=%s, offset=%lld, length=%lld, "
        "target_size=%lld",
        file_info.patch_identifier_.c_str(),
        file_info.patch_data_offset_,
        file_info.patch_data_length_,
        file_info.target_size_);

    fs::path output_path = fs::path(output_dir_) / file_info.target_path_;
    std::string temp_output_path = output_path.string() + "_tmp";

    FileSliceStream patch_slice(patch_path.string(),
                                file_info.patch_data_offset_,
                                file_info.patch_data_length_);

    // All patches are in HDiffPatch format (even new files have oldDataSize=0)
    // For new files, hpatchz requires an existing (but empty) source file
    std::string empty_source = temp_output_path + ".empty_src";
    {
      std::ofstream empty_file(empty_source, std::ios::binary);
      empty_file.close();
    }

    LOG_DEBUG(
        "Applying HDiffPatch (compressed=%s) for new file with empty source",
        file_info.is_compressed_ ? "yes" : "no");

    if (!apply_hdiff_patch(&empty_source,
                           patch_slice,
                           temp_output_path,
                           file_info.is_compressed_)) {
      LOG_ERROR("Failed to apply HDiffPatch for new file");
      // Clean up temporary files on failure
      fs::remove(empty_source);
      if (fs::exists(temp_output_path)) {
        fs::remove(temp_output_path);
      }
      return false;
    }

    // Clean up temporary empty source file
    fs::remove(empty_source);

    if (fs::exists(output_path)) {
      fs::remove(output_path);
    }
    fs::rename(temp_output_path, output_path);

    return true;

  } catch (const std::exception& e) {
    LOG_ERROR("Exception in create_new_file: %s", e.what());
    // Clean up any temporary files on exception
    namespace fs = std::filesystem;
    fs::path output_path = fs::path(output_dir_) / file_info.target_path_;
    std::string temp_output_path = output_path.string() + "_tmp";
    std::string empty_source = temp_output_path + ".empty_src";
    std::error_code ec;
    fs::remove(temp_output_path, ec);
    fs::remove(empty_source, ec);
    return false;
  }
}

bool PatchApplier::update_existing_file(const PatchFileMetadata& file_info) {
  try {
    namespace fs = std::filesystem;

    std::string source_file_name = file_info.source_file_name_;
    if (!package_name_.empty() &&
        source_file_name.find(package_name_ + "/") == 0) {
      source_file_name = source_file_name.substr(package_name_.length() + 1);
      LOG_DEBUG("Stripped package prefix '%s/' from source path: %s",
                package_name_.c_str(),
                source_file_name.c_str());
    } else if (!package_name_.empty() &&
               source_file_name.find(package_name_ + "\\") == 0) {
      source_file_name = source_file_name.substr(package_name_.length() + 1);
      LOG_DEBUG("Stripped package prefix '%s\\' from source path: %s",
                package_name_.c_str(),
                source_file_name.c_str());
    }

    fs::path source_path = fs::path(output_dir_) / source_file_name;

    if (!fs::exists(source_path)) {
      LOG_ERROR("Source file not found: %s", source_path.string().c_str());
      return false;
    }

    // Verify source file MD5 if provided
    if (!file_info.source_file_md5_.empty()) {
      std::string source_path_str = source_path.string();
      if (!verify_file_md5(source_path_str, file_info.source_file_md5_)) {
        LOG_ERROR("Source file MD5 verification failed before patching: %s",
                  source_file_name.c_str());
        LOG_ERROR("  This indicates the game files are not at version %s",
                  file_info.source_version_tag_.c_str());
        LOG_ERROR("  Please verify game integrity or use full download");
        return false;
      }
      LOG_DEBUG("Source file MD5 verified: %s", source_file_name.c_str());
    }

    fs::path patch_path = fs::path(ldiff_dir_) / file_info.patch_identifier_;

    FileSliceStream patch_slice(patch_path.string(),
                                file_info.patch_data_offset_,
                                file_info.patch_data_length_);

    fs::path output_path = fs::path(output_dir_) / file_info.target_path_;
    std::string temp_output_path = output_path.string() + "_tmp";

    std::string source_path_str = source_path.string();
    if (!apply_hdiff_patch(
            &source_path_str, patch_slice, temp_output_path, false)) {
      // Clean up temporary file on failure
      if (fs::exists(temp_output_path)) {
        fs::remove(temp_output_path);
      }
      return false;
    }

    if (fs::exists(output_path)) {
      fs::remove(output_path);
    }
    fs::rename(temp_output_path, output_path);

    if (source_path != output_path) {
      if (fs::exists(source_path)) {
        fs::remove(source_path);
        LOG_INFO("Removed old file: %s", source_path.c_str());
      }
    }

    return true;

  } catch (const std::exception& e) {
    LOG_ERROR("Exception in update_existing_file: %s", e.what());
    // Clean up temporary file on exception
    namespace fs = std::filesystem;
    fs::path output_path = fs::path(output_dir_) / file_info.target_path_;
    std::string temp_output_path = output_path.string() + "_tmp";
    std::error_code ec;
    fs::remove(temp_output_path, ec);
    return false;
  }
}

bool PatchApplier::apply_hdiff_patch(const std::string* source_file,
                                     FileSliceStream& patch_slice,
                                     const std::string& output_file,
                                     bool is_compressed) {
  try {
    std::vector<uint8_t> patch_data(
        static_cast<size_t>(patch_slice.get_remaining_bytes()));
    size_t read_size = patch_slice.read(patch_data.data(), patch_data.size());

    if (read_size != patch_data.size()) {
      LOG_ERROR("Failed to read patch data completely");
      return false;
    }

    // All patch files (both new and updates) are in HDiffPatch format.
    // For new files, HDiffPatch has oldDataSize=0 and creates file from
    // scratch. The is_compressed flag indicates whether the HDiffPatch payload
    // uses zstd.
    //
    // From hpatchz -info output:
    //   diffDataType: HDiff
    //   saved oldDataSize: 0 (for new files) or >0 (for updates)
    //   saved newDataSize: <target size>
    //   compressType: "zstd" (when is_compressed=true)

    auto result = HPatchUtils::apply_patch_from_memory(source_file,
                                                       patch_data.data(),
                                                       patch_data.size(),
                                                       output_file,
                                                       is_compressed);

    if (!result.success) {
      LOG_ERROR("Failed to apply HDiffPatch: %s", result.error_message.c_str());
      return false;
    }

    return true;

  } catch (const std::exception& e) {
    LOG_ERROR("Exception in apply_hdiff_patch: %s", e.what());
    return false;
  }
}

bool PatchApplier::copy_patch_data(FileSliceStream& patch_slice,
                                   const std::string& output_file) {
  try {
    std::ofstream out_file(output_file, std::ios::binary);
    if (!out_file.is_open()) {
      LOG_ERROR("Failed to open output file: %s", output_file.c_str());
      return false;
    }

    constexpr size_t k_buffer_size = 64 * 1024;  // 64KB buffer
    std::vector<char> buffer(k_buffer_size);

    while (!patch_slice.is_eof()) {
      size_t bytes_read = patch_slice.read(
          reinterpret_cast<uint8_t*>(buffer.data()), buffer.size());
      if (bytes_read > 0) {
        out_file.write(buffer.data(), bytes_read);
      }
    }

    out_file.close();
    return out_file.good();

  } catch (const std::exception& e) {
    LOG_ERROR("Exception in copy_patch_data: %s", e.what());
    return false;
  }
}

bool PatchApplier::verify_file_md5(std::string_view file_path,
                                   std::string_view expected_md5) {
  try {
    if (file_path.empty()) {
      LOG_ERROR("File path is empty");
      return false;
    }
    auto res = DownloadUtils::VerifyDownloadedFile(
        std::string(file_path), 0, std::string(expected_md5));
    if (!res.md5_match) {
      LOG_ERROR("MD5 mismatch for %s: expected %s, got %s",
                std::string(file_path).c_str(),
                std::string(expected_md5).c_str(),
                res.actual_md5.c_str());
      return false;
    }
    return true;

  } catch (const std::exception& e) {
    LOG_ERROR("Exception in verify_file_md5: %s", e.what());
    return false;
  }
}

bool PatchApplier::delete_obsolete_files(
    const std::shared_ptr<PatchManifestProcessor>& manifest,
    std::string* locked_file_out) {
  if (!manifest) {
    LOG_ERROR("Manifest is null");
    throw std::invalid_argument("Manifest cannot be null");
  }

  const auto& delete_files = manifest->get_obsolete_file_list();

  // Retry deletions recorded by a previous run first: a file that was locked
  // (e.g. still loaded by another process) is unlikely to be unlocked in
  // seconds, so deferring it to the next update run is safer than failing the
  // whole update. Missing entries are dropped silently (already gone).
  std::vector<std::pair<std::string, std::string>> deferred;
  {
    for (const std::string& rel : load_pending_deletions()) {
      try {
        const fs::path file_path = fs::path(output_dir_) / rel;
        if (!fs::exists(file_path)) {
          continue;  // already removed since the failed attempt
        }
        std::error_code ec;
        fs::remove(file_path, ec);
        if (ec) {
          if (is_lock_error(ec)) {
            LOG_ERROR("Deferred file %s is still locked by another process",
                      rel.c_str());
            if (locked_file_out) {
              *locked_file_out = rel;
            }
            return false;
          }
          deferred.emplace_back(rel, ec.message());
        }
      } catch (const std::exception& e) {
        deferred.emplace_back(rel, e.what());
      }
    }
  }

  const size_t prior = delete_files.size();
  LOG_INFO("Deleting %zu obsolete files (%zu previous deferred retries)",
           prior,
           deferred.size());

  int deleted_count = 0;
  int failed_count = 0;
  std::vector<std::string> pending_rel;
  std::vector<std::string> locked_rel;

  // Delete files with a short retry window; failures are persisted so the
  // next update run can retry them, never aborting the update. Locked files
  // are reported to the caller instead, since only the user can release them.
  auto delete_with_retry = [this](const fs::path& file_path,
                                  std::string* error) -> DeleteResult {
    constexpr int kMaxAttempts = 3;  // brief window for transient locks
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
      std::error_code ec;
      fs::remove(file_path, ec);
      if (!ec) {
        return DeleteResult::kDeleted;
      }
      if (attempt == 1 && is_lock_error(ec)) {
        return DeleteResult::kLocked;
      }
      if (attempt < kMaxAttempts) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
      if (error) {
        *error = ec.message();
      }
    }
    return DeleteResult::kGenericFail;
  };

  // 1. Retry previously deferred deletions first (they are stale by definition
  //    but worth one more chance before this run's set is merged).
  for (auto& entry : deferred) {
    const fs::path file_path = fs::path(output_dir_) / entry.first;
    if (!fs::exists(file_path)) {
      continue;
    }
    std::string error;
    switch (delete_with_retry(file_path, &error)) {
      case DeleteResult::kDeleted:
        deleted_count++;
        break;
      case DeleteResult::kLocked:
        locked_rel.push_back(entry.first);
        break;
      default:
        LOG_WARN("Failed to delete deferred file %s (%s); still deferred",
                 entry.first.c_str(),
                 error.c_str());
        failed_count++;
        pending_rel.push_back(entry.first);
        break;
    }
  }

  // 2. Delete this manifest's obsolete files.
  for (const auto& delete_info : delete_files) {
    try {
      fs::path file_path = fs::path(output_dir_) / delete_info.file_path_;

      if (fs::exists(file_path)) {
        std::string file_path_str = file_path.string();
        if (!delete_info.file_md5_.empty()) {
          if (!verify_file_md5(file_path_str, delete_info.file_md5_)) {
            LOG_WARN(
                "MD5 mismatch for file to be deleted: %s (deleting anyway as "
                "marked obsolete)",
                delete_info.file_path_.c_str());
          }
        }

        std::string error;
        switch (delete_with_retry(file_path, &error)) {
          case DeleteResult::kDeleted:
            deleted_count++;
            break;
          case DeleteResult::kLocked:
            locked_rel.push_back(delete_info.file_path_);
            break;
          default:
            LOG_WARN("Failed to delete file %s (%s); will retry next update",
                     delete_info.file_path_.c_str(),
                     error.c_str());
            failed_count++;
            pending_rel.push_back(delete_info.file_path_);
            break;
        }
      } else {
        LOG_DEBUG("File not found (already deleted?): %s",
                  delete_info.file_path_.c_str());
      }

    } catch (const std::exception& e) {
      LOG_WARN("Failed to delete file %s (%s); will retry next update",
               delete_info.file_path_.c_str(),
               e.what());
      failed_count++;
      pending_rel.push_back(delete_info.file_path_);
    }
  }

  // Persist whatever still could not be removed for a later retry. Deferred
  // entries that succeeded this run are naturally dropped; failures from both
  // sources are merged.
  save_pending_deletions(pending_rel);

  LOG_INFO("Deleted %d files, %d failed (deferred for next update)",
           deleted_count,
           failed_count);

  // Locked files cannot be removed now; register a logon-time cleanup and
  // report the first offender so the caller can resolve the lock and retry.
  if (!locked_rel.empty()) {
    if (schedule_runonce_deletion(locked_rel)) {
      LOG_INFO("Scheduled %zu locked file(s) for deletion at next logon",
               locked_rel.size());
    }
    if (locked_file_out) {
      *locked_file_out = locked_rel.front();
    }
    return false;
  }

  // Deletion is best-effort: the update itself already applied. Report the
  // failure count but never treat leftovers as a fatal error.
  return true;
}

bool PatchApplier::schedule_runonce_deletion(
    const std::vector<std::string>& relative_paths) {
#ifdef QUATON_PLATFORM_WINDOWS
  if (relative_paths.empty()) {
    return true;
  }
  try {
    const fs::path script_dir = fs::path(PathHelper::GetDataDir());
    const fs::path script_path = script_dir / "pending_delete.ps1";
    fs::create_directories(script_dir);

    // PowerShell handles Unicode paths in .ps1 reliably; write with a UTF-8
    // BOM so Windows PowerShell 5.1 decodes non-ASCII paths correctly. One
    // self-contained statement per path avoids array-literal parsing quirks.
    std::string script;
    script +=
        "try { Remove-Item -LiteralPath $PSCommandPath -Force -ErrorAction "
        "SilentlyContinue } catch {}\n";
    for (const auto& rel : relative_paths) {
      const std::string full =
          escape_single_quote((fs::path(output_dir_) / rel).generic_string());
      script += "if (Test-Path -LiteralPath '" + full +
                "') { Remove-Item -LiteralPath '" + full +
                "' -Force -ErrorAction SilentlyContinue }\n";
    }
    script +=
        "reg.exe delete "
        "\"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce\" /v "
        "QuatonCleanupDeletions /f 2>$null\n";

    std::ofstream out(script_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      LOG_ERROR("Failed to open cleanup script: %s",
                script_path.string().c_str());
      return false;
    }
    const uint8_t bom[] = {0xEF, 0xBB, 0xBF};
    out.write(reinterpret_cast<const char*>(bom), sizeof(bom));
    out << script;
    out.flush();
    if (!out.good()) {
      LOG_ERROR("Failed to write cleanup script: %s",
                script_path.string().c_str());
      return false;
    }

    const std::wstring cmd =
        L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" +
        script_path.wstring() + L"\"";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
                      0,
                      KEY_SET_VALUE,
                      &key) != ERROR_SUCCESS) {
      LOG_ERROR("Failed to open HKCU RunOnce key");
      return false;
    }
    const wchar_t* value = cmd.c_str();
    const LONG res =
        RegSetValueExW(key,
                       L"QuatonCleanupDeletions",
                       0,
                       REG_SZ,
                       reinterpret_cast<const BYTE*>(value),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (res != ERROR_SUCCESS) {
      LOG_ERROR("Failed to set RunOnce value: %ld", static_cast<long>(res));
      return false;
    }
    LOG_INFO("Registered logon cleanup task: %s", script_path.string().c_str());
    return true;
  } catch (const std::exception& e) {
    LOG_ERROR("Failed to schedule logon deletion: %s", e.what());
    return false;
  }
#else
  (void)relative_paths;
  return true;  // no-op on non-Windows
#endif
}

void PatchApplier::save_pending_deletions(
    const std::vector<std::string>& relative_paths) {
  try {
    const fs::path queue_path = fs::path(ldiff_dir_) / "pending_deletions.json";
    fs::create_directories(ldiff_dir_);

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& rel : relative_paths) {
      arr.push_back(rel);
    }

    std::ofstream out(queue_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      LOG_ERROR("Failed to open pending deletions file: %s",
                queue_path.string().c_str());
      return;
    }
    out << arr.dump(2);
    out.flush();
    if (!out.good()) {
      LOG_ERROR("Failed to write pending deletions file: %s",
                queue_path.string().c_str());
      return;
    }
  } catch (const std::exception& e) {
    LOG_ERROR("Failed to save pending deletions: %s", e.what());
  }
}

std::vector<std::string> PatchApplier::load_pending_deletions() const {
  std::vector<std::string> result;
  const fs::path queue_path = fs::path(ldiff_dir_) / "pending_deletions.json";
  try {
    std::ifstream in(queue_path, std::ios::binary);
    if (!in) {
      return result;
    }
    nlohmann::json arr;
    in >> arr;
    if (!arr.is_array()) {
      return result;
    }
    for (const auto& item : arr) {
      if (item.is_string()) {
        result.push_back(item.get<std::string>());
      }
    }
  } catch (const std::exception& e) {
    LOG_ERROR("Failed to load pending deletions: %s", e.what());
  }
  return result;
}

bool PatchApplier::cleanup_ldiff_directory() {
  try {
    if (!fs::exists(ldiff_dir_)) {
      LOG_INFO("ldiff directory does not exist: %s", ldiff_dir_.c_str());
      return true;
    }

    LOG_INFO("Cleaning up ldiff directory: %s", ldiff_dir_.c_str());

    fs::remove_all(ldiff_dir_);

    LOG_INFO("ldiff directory cleaned up successfully");
    return true;

  } catch (const std::exception& e) {
    LOG_ERROR("Failed to cleanup ldiff directory: %s", e.what());
    return false;
  }
}

}  // namespace Quaton