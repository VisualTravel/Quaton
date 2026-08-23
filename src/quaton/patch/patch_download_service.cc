#include "quaton/patch/patch_download_service.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "quaton/downloader/downloader_utils.h"
#include "quaton/http_client.h"
#include "quaton/logger.h"
#include "quaton/manifest/manifest_processor_patch.h"
#include "quaton/patch.h"
#include "quaton/patch_applier.h"
#include "quaton/utils/checksum.h"
#include "quaton/utils/hpatch.h"
#include "quaton/utils/path_helper.h"

#ifdef _WIN32
#include <conio.h>
#else
#include <sys/select.h>
#include <sys/time.h>
#endif

namespace fs = std::filesystem;

// Exit trigger helper function (in anonymous namespace to avoid conflicts)
namespace {
void PatchServiceExitTrigger(std::shared_ptr<std::atomic<bool>> cancel_flag) {
  while (!*cancel_flag) {
#ifdef _WIN32
    if (_kbhit()) {
      int ch = _getch();
      if (ch == 'c' || ch == 'C') {
        *cancel_flag = true;
        LOG_INFO("Patch download cancelled by user");
        break;
      } else if (ch == 'r' || ch == 'R') {
        LOG_INFO("Patch download restart requested");
        *cancel_flag = true;
        break;
      }
    }
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

// Helper function to log operation failure and set error flag
void LogOperationFailure(const std::string& operation,
                         const std::string& identifier,
                         std::atomic<bool>& error_flag,
                         const std::string& additional_info = "") {
  if (additional_info.empty()) {
    LOG_ERROR("Failed to %s: %s", operation.c_str(), identifier.c_str());
  } else {
    LOG_ERROR("Failed to %s: %s (%s)",
              operation.c_str(),
              identifier.c_str(),
              additional_info.c_str());
  }
  error_flag = true;
}
}  // namespace

QUATON_NAMESPACE_BEGIN

PatchDownloadService::PatchDownloadService() = default;

PatchDownloadService::~PatchDownloadService() {
  Cancel();

  // Wait for download workers to finish
  for (auto& worker : download_workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }

  // Wait for apply workers to finish
  for (auto& worker : apply_workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }

  Shutdown();
}

bool PatchDownloadService::Initialize(const PatchServiceConfig& config) {
  patch_service_config_ = config;
  config_ = config;

  mode_ = config.mode;
  download_threads_ = config.download_threads;
  apply_threads_ = config.apply_threads;
  max_buffer_size_ = config.max_buffer_size;

  // Auto-detect thread counts
  if (download_threads_ <= 0) {
    download_threads_ = (std::max)(1u, std::thread::hardware_concurrency() / 2);
  }
  if (apply_threads_ <= 0) {
    apply_threads_ = (std::max)(1u, std::thread::hardware_concurrency() / 2);
  }

  download_threads_ = (std::max)(1, download_threads_);
  apply_threads_ = (std::max)(1, apply_threads_);
  max_buffer_size_ = (std::max)(kPatchMinBufferSize, max_buffer_size_);

  if (!DownloadService::Initialize()) {
    LOG_ERROR("[PatchDownloadService] Base initialization failed");
    return false;
  }

  http_client_ = std::make_shared<HttpClient>();

  patch_manager_ = std::make_shared<PatchDownloadManager>(http_client_);

  if (!patch_manager_->Initialize(config.manager_config)) {
    LOG_ERROR("[PatchDownloadService] Manager initialization failed");
    return false;
  }

  if (config.create_backup && !config.backup_path.empty()) {
    std::error_code ec;
    fs::create_directories(config.backup_path, ec);
    if (ec) {
      LOG_WARN("[PatchDownloadService] Failed to create backup dir: %s",
               ec.message().c_str());
    }
  }

  if (!config.temp_patch_dir.empty()) {
    PathHelper::CreateDirectoryRecursive(config.temp_patch_dir);
  }

  output_dir_ = config.manager_config.target_path;

  LOG_INFO("[PatchDownloadService] Initialized successfully");
  LOG_INFO("  Mode: %s",
           mode_ == PatchDownloadMode::kPreDownload ? "PreDownload"
                                                    : "DownloadAndApply");
  LOG_INFO("  Download threads: %d", download_threads_);
  LOG_INFO("  Apply threads: %d", apply_threads_);
  LOG_INFO("  Max buffer size: %d", max_buffer_size_);

  return true;
}

void PatchDownloadService::SetProgressCallback(ProgressCallback callback) {
  progress_callback_ = std::move(callback);
  if (progress_tracker_) {
    progress_tracker_->set_callback([this](const ProgressInfo& info) {
      if (progress_callback_) {
        progress_callback_(info);
      }
    });
  }
}

int PatchDownloadService::PrepareDownloadTasks(
    const std::shared_ptr<PatchManifestProcessor>& manifest,
    const std::string& package_name,
    const std::string& download_url_prefix,
    const std::string& download_url_suffix) {
  if (!manifest) {
    LOG_ERROR("Manifest is null");
    return 0;
  }

  manifest_ = manifest;
  package_name_ = package_name;

  std::vector<std::string> unique_patch_ids =
      manifest_->get_distinct_patch_identifiers();

  LOG_INFO("Preparing %zu unique patch files for download",
           unique_patch_ids.size());

  patch_items_.clear();
  patch_items_.reserve(unique_patch_ids.size());

  std::unordered_map<std::string, size_t> patch_id_to_index;

  std::filesystem::path package_dir =
      std::filesystem::path(patch_service_config_.temp_patch_dir) /
      package_name;
  std::string package_dir_str = package_dir.string();

  for (const auto& patch_id : unique_patch_ids) {
    int64_t size = 0;
    std::string md5;

    if (!manifest_->retrieve_patch_details(patch_id, size, md5)) {
      LOG_ERROR("Failed to get patch info for: %s", patch_id.c_str());
      continue;
    }

    std::string url = DownloadUtils::BuildDownloadUrl(
        download_url_prefix, patch_id, download_url_suffix);

    std::string save_path =
        DownloadUtils::BuildFilePath(package_dir_str, patch_id);

    PipelinePatchItem item(patch_id, url, save_path, size, md5);

    size_t index = patch_items_.size();
    patch_id_to_index[patch_id] = index;
    patch_items_.push_back(item);
  }

  // Associate files with patches
  const auto& patch_files = manifest_->get_patch_file_list();
  for (const auto& file_info : patch_files) {
    auto it = patch_id_to_index.find(file_info.patch_identifier_);
    if (it != patch_id_to_index.end()) {
      patch_items_[it->second].associated_files.push_back(&file_info);
    }
  }

  int64_t total_bytes = 0;
  int total_target_files = 0;
  for (const auto& item : patch_items_) {
    total_bytes += item.file_size;
    total_target_files += static_cast<int>(item.associated_files.size());
  }
  total_bytes_ = total_bytes;

  // Update progress tracker with correct file counts
  OperationMode op_mode = (mode_ == PatchDownloadMode::kPreDownload)
                              ? OperationMode::kPatchPredownload
                              : OperationMode::kPatchUpdate;
  progress_tracker_ = std::make_unique<ProgressTracker>(
      mode_ == PatchDownloadMode::kDownloadAndApply
          ? total_target_files
          : static_cast<int>(patch_items_.size()),
      op_mode);

  if (progress_callback_) {
    progress_tracker_->set_callback(progress_callback_);
  }

  LOG_INFO("Prepared %zu patch items with %zu target files (%.2f MB total)",
           patch_items_.size(),
           patch_files.size(),
           total_bytes / (1024.0 * 1024.0));

  return static_cast<int>(patch_items_.size());
}

std::future<int> PatchDownloadService::Execute() {
  return std::async(std::launch::async, [this]() -> int {
    if (patch_items_.empty()) {
      LOG_INFO("No patches to download");
      return 0;
    }

    cancelled_ = false;
    has_error_ = false;
    downloaded_count_ = 0;
    applied_count_ = 0;
    downloaded_bytes_ = 0;
    buffer_count_ = 0;

    LOG_INFO("Starting pipeline execution with %zu patches",
             patch_items_.size());

    // Populate download queue
    {
      std::lock_guard<std::mutex> lock(download_queue_mutex_);
      for (size_t i = 0; i < patch_items_.size(); ++i) {
        download_queue_.push(i);
      }
    }

    // Start download workers
    download_workers_.clear();
    download_workers_.reserve(download_threads_);
    for (int i = 0; i < download_threads_; ++i) {
      download_workers_.emplace_back([this]() { DownloadWorker(); });
    }

    // If DownloadAndApply mode, start apply threads
    if (mode_ == PatchDownloadMode::kDownloadAndApply) {
      apply_workers_.clear();
      apply_workers_.reserve(apply_threads_);
      for (int i = 0; i < apply_threads_; ++i) {
        apply_workers_.emplace_back([this]() { ApplyWorker(); });
      }
    }

    // Wait for download workers
    for (auto& worker : download_workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    download_workers_.clear();

    LOG_INFO("All download threads completed");

    // Wait for apply workers if in DownloadAndApply mode
    if (mode_ == PatchDownloadMode::kDownloadAndApply) {
      apply_cv_.notify_all();

      for (auto& worker : apply_workers_) {
        if (worker.joinable()) {
          worker.join();
        }
      }
      apply_workers_.clear();

      LOG_INFO("All apply threads completed");
    }

    if (cancelled_) {
      LOG_INFO("Pipeline execution cancelled");
      return -1;
    }

    if (has_error_) {
      LOG_ERROR("Pipeline execution failed with errors");
      return -2;
    }

    LOG_INFO("Pipeline execution completed successfully");
    LOG_INFO("Downloaded: %d patches, Applied: %d files",
             downloaded_count_.load(),
             applied_count_.load());
    LOG_INFO("Total bytes downloaded: %.2f MB",
             downloaded_bytes_.load() / (1024.0 * 1024.0));

    return 0;
  });
}

std::future<int> PatchDownloadService::ApplyDownloadedPatches() {
  return std::async(std::launch::async, [this]() -> int {
    if (mode_ != PatchDownloadMode::kPreDownload) {
      LOG_ERROR(
          "ApplyDownloadedPatches can only be called in PreDownload mode");
      return -1;
    }

    LOG_INFO("Starting to apply downloaded patches");

    cancelled_ = false;
    has_error_ = false;
    applied_count_ = 0;

    // Prepare apply queue with all downloaded patches
    {
      std::lock_guard<std::mutex> lock(apply_queue_mutex_);
      for (size_t i = 0; i < patch_items_.size(); ++i) {
        if (patch_items_[i].download_completed &&
            !patch_items_[i].apply_completed) {
          apply_queue_.push(i);
        }
      }
    }

    // Start worker threads
    apply_workers_.clear();
    apply_workers_.reserve(apply_threads_);
    for (int i = 0; i < apply_threads_; ++i) {
      apply_workers_.emplace_back([this]() { ApplyWorker(); });
    }

    // Wait for completion
    for (auto& worker : apply_workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    apply_workers_.clear();

    if (cancelled_) {
      LOG_INFO("Apply process cancelled");
      return -1;
    }

    if (has_error_) {
      LOG_ERROR("Apply process failed with errors");
      return -2;
    }

    LOG_INFO("Successfully applied all patches");
    return 0;
  });
}

void PatchDownloadService::Cancel() {
  cancelled_ = true;
  download_cv_.notify_all();
  apply_cv_.notify_all();
}

// ========== Pipeline Workers ==========

void PatchDownloadService::DownloadWorker() {
  while (!cancelled_ && !has_error_) {
    PipelinePatchItem* item = nullptr;

    // Check if can start new download
    {
      std::unique_lock<std::mutex> lock(download_queue_mutex_);

      // Wait for buffer space or queue not empty or all tasks complete
      download_cv_.wait(lock, [this]() {
        return cancelled_ || has_error_ ||
               (CanStartNewDownload() && !download_queue_.empty()) ||
               download_queue_.empty();
      });

      if (cancelled_ || has_error_) {
        LOG_DEBUG("Download worker exiting due to cancellation or error");
        return;
      }

      if (download_queue_.empty()) {
        LOG_DEBUG("Download worker exiting: queue is empty");
        return;
      }

      size_t index = download_queue_.front();
      download_queue_.pop();

      {
        std::lock_guard<std::mutex> items_lock(items_mutex_);
        if (index < patch_items_.size()) {
          item = &patch_items_[index];
          item->download_started = true;
        }
      }
    }

    if (!item) {
      continue;
    }

    if (progress_tracker_) {
      progress_tracker_->update_current_file(
          item->patch_identifier, FileProcessStage::kDownloadInitiated);
    }

    LOG_INFO("Downloading patch: %s (%.2f MB)",
             item->patch_identifier.c_str(),
             item->file_size / (1024.0 * 1024.0));

    const int max_retries = 3;
    bool success = false;

    for (int retry = 0; retry < max_retries && !cancelled_; ++retry) {
      if (retry > 0) {
        LOG_WARN("Retrying download (%d/%d): %s",
                 retry + 1,
                 max_retries,
                 item->patch_identifier.c_str());
        std::this_thread::sleep_for(std::chrono::seconds(2));
      }

      if (DownloadSinglePatch(*item)) {
        success = true;
        break;
      }
    }

    if (!success) {
      std::string retry_info = std::to_string(max_retries) + " retries";
      LogOperationFailure("download patch",
                          item->patch_identifier,
                          has_error_,
                          "after " + retry_info);
      return;
    }

    MarkDownloadCompleted(*item);

    if (progress_tracker_) {
      progress_tracker_->update_current_file(
          item->patch_identifier, FileProcessStage::kDownloadCompleted);
      progress_tracker_->update_download_stats(downloaded_bytes_.load(),
                                               total_bytes_);
    }

    LOG_INFO("Downloaded patch: %s (%d/%d)",
             item->patch_identifier.c_str(),
             downloaded_count_.load(),
             static_cast<int>(patch_items_.size()));

    // If DownloadAndApply mode, add task to apply queue
    if (mode_ == PatchDownloadMode::kDownloadAndApply) {
      size_t index = item - &patch_items_[0];
      {
        std::lock_guard<std::mutex> lock(apply_queue_mutex_);
        apply_queue_.push(index);
      }
      apply_cv_.notify_one();
    } else {
      // In PreDownload mode, release buffer immediately after download
      buffer_count_--;
      download_cv_.notify_one();
    }
  }
}

void PatchDownloadService::ApplyWorker() {
  while (!cancelled_ && !has_error_) {
    PipelinePatchItem* item = nullptr;
    size_t index = 0;

    {
      std::unique_lock<std::mutex> lock(apply_queue_mutex_);

      apply_cv_.wait(lock, [this]() {
        return cancelled_ || has_error_ || !apply_queue_.empty() ||
               (downloaded_count_.load() >=
                    static_cast<int>(patch_items_.size()) &&
                apply_queue_.empty());
      });

      if (cancelled_ || has_error_) {
        return;
      }

      if (apply_queue_.empty()) {
        if (downloaded_count_.load() >= static_cast<int>(patch_items_.size())) {
          LOG_DEBUG(
              "Apply worker exiting: all downloads complete and queue empty");
          return;
        }
        continue;
      }

      index = apply_queue_.front();
      apply_queue_.pop();

      {
        std::lock_guard<std::mutex> items_lock(items_mutex_);
        if (index < patch_items_.size()) {
          item = &patch_items_[index];
        }
      }
    }

    if (!item) {
      continue;
    }

    LOG_INFO("Applying patch: %s (%zu target files)",
             item->patch_identifier.c_str(),
             item->associated_files.size());

    // Apply patch
    if (!ApplySinglePatch(*item)) {
      LogOperationFailure("apply patch", item->patch_identifier, has_error_);
      return;
    }

    // Mark apply completed
    MarkApplyCompleted(*item);

    // Cleanup patch file
    CleanupAppliedPatch(*item);

    LOG_INFO("Applied patch: %s (%d target files completed)",
             item->patch_identifier.c_str(),
             applied_count_.load());
  }
}

// ========== Download Operations ==========

bool PatchDownloadService::DownloadSinglePatch(PipelinePatchItem& item) {
  try {
    if (fs::exists(item.storage_path)) {
      if (VerifyDownloadedFile(item)) {
        LOG_INFO("Patch file already exists and verified: %s",
                 item.patch_identifier.c_str());
        return true;
      } else {
        LOG_WARN("Existing file failed verification, re-downloading: %s",
                 item.patch_identifier.c_str());
        fs::remove(item.storage_path);
      }
    }

    // Download file
    if (!DownloadUtils::DownloadToFile(http_client_,
                                       item.download_url,
                                       item.storage_path,
                                       item.file_size,
                                       item.checksum_md5)) {
      LOG_ERROR("Failed to download patch: %s", item.patch_identifier.c_str());
      return false;
    }

    item.verified = true;
    return true;

  } catch (const std::exception& e) {
    LOG_ERROR("Exception in DownloadSinglePatch: %s", e.what());
    return false;
  }
}

bool PatchDownloadService::VerifyDownloadedFile(const PipelinePatchItem& item) {
  return DownloadUtils::IsFileExistAndValid(
      item.storage_path, item.file_size, item.checksum_md5);
}

bool PatchDownloadService::CanStartNewDownload() const {
  return buffer_count_.load() < max_buffer_size_;
}

void PatchDownloadService::MarkDownloadCompleted(PipelinePatchItem& item) {
  std::lock_guard<std::mutex> lock(items_mutex_);
  item.download_completed = true;
  downloaded_count_++;
  downloaded_bytes_ += item.file_size;
  buffer_count_++;
}

// ========== Apply Operations ==========

bool PatchDownloadService::ApplySinglePatch(PipelinePatchItem& item) {
  if (item.associated_files.empty()) {
    LOG_WARN("No associated files for patch: %s",
             item.patch_identifier.c_str());
    return true;
  }

  for (const auto* file_info : item.associated_files) {
    if (cancelled_) {
      return false;
    }

    LOG_INFO("Applying patch to: %s", file_info->target_path_.c_str());

    if (!ApplyPatchToFile(*file_info, item.storage_path)) {
      LOG_ERROR("Failed to apply patch to file: %s",
                file_info->target_path_.c_str());
      return false;
    }

    applied_count_++;

    // Callback
    if (patch_applied_callback_) {
      patch_applied_callback_(file_info->target_path_, true);
    }
  }

  return true;
}

bool PatchDownloadService::ApplyPatchToFile(const PatchFileMetadata& file_info,
                                            const std::string& patch_path) {
  try {
    // Normalize path
    std::string normalized_target = file_info.target_path_;
#ifdef _WIN32
    std::replace(normalized_target.begin(), normalized_target.end(), '/', '\\');
#endif
    fs::path target_file_path = fs::path(output_dir_) / normalized_target;
    std::string target_path = target_file_path.string();

    if (target_file_path.has_parent_path()) {
      std::error_code ec;
      fs::path parent_dir = target_file_path.parent_path();

      if (!fs::exists(parent_dir, ec)) {
        if (!fs::create_directories(parent_dir, ec)) {
          LOG_ERROR("Failed to create directory: %s (error: %s)",
                    parent_dir.string().c_str(),
                    ec.message().c_str());
          return false;
        }
      }
    }

    if (progress_tracker_) {
      progress_tracker_->update_current_file(file_info.target_path_,
                                             FileProcessStage::kDecompressed);
    }

    bool result = false;
    if (file_info.is_new_file_) {
      result = CreateNewFileFromPatch(file_info, patch_path, target_path);
    } else {
      result = UpdateExistingFile(file_info, patch_path, target_path);
    }

    if (result && progress_tracker_) {
      progress_tracker_->update_current_file(file_info.target_path_,
                                             FileProcessStage::kApplied);
      progress_tracker_->increment_completed();
    }

    return result;

  } catch (const std::exception& e) {
    LOG_ERROR("Exception in ApplyPatchToFile: %s", e.what());
    return false;
  }
}

bool PatchDownloadService::CreateNewFileFromPatch(
    const PatchFileMetadata& file_info,
    const std::string& patch_path,
    const std::string& target_path) {
  try {
    FileSliceStream patch_slice(
        patch_path, file_info.patch_data_offset_, file_info.patch_data_length_);

    std::string temp_output = target_path + "_tmp";

    // Apply HDiffPatch with no source file
    if (!ApplyHDiffPatchInternal(nullptr, patch_slice, temp_output, false)) {
      if (fs::exists(temp_output)) {
        fs::remove(temp_output);
      }
      return false;
    }

    // Verify MD5
    if (!file_info.target_md5_.empty()) {
      std::string actual_md5 = ChecksumUtils::calculate_md5_file(temp_output);
      if (actual_md5 != file_info.target_md5_) {
        LOG_ERROR(
            "MD5 verification failed for new file: %s (expected: %s, got: %s)",
            file_info.target_path_.c_str(),
            file_info.target_md5_.c_str(),
            actual_md5.c_str());
        fs::remove(temp_output);
        return false;
      }
    }

    // Rename to final location
    if (fs::exists(target_path)) {
      fs::remove(target_path);
    }
    fs::rename(temp_output, target_path);

    LOG_INFO("Successfully created new file: %s",
             file_info.target_path_.c_str());
    return true;

  } catch (const std::exception& e) {
    LOG_ERROR("Exception in CreateNewFileFromPatch: %s", e.what());
    std::string temp_output = target_path + "_tmp";
    std::error_code ec;
    fs::remove(temp_output, ec);
    return false;
  }
}

bool PatchDownloadService::UpdateExistingFile(
    const PatchFileMetadata& file_info,
    const std::string& patch_path,
    const std::string& target_path) {
  try {
    // Handle package name prefix
    std::string source_file_name = file_info.source_file_name_;
    if (!package_name_.empty() &&
        source_file_name.find(package_name_ + "/") == 0) {
      source_file_name = source_file_name.substr(package_name_.length() + 1);
    } else if (!package_name_.empty() &&
               source_file_name.find(package_name_ + "\\") == 0) {
      source_file_name = source_file_name.substr(package_name_.length() + 1);
    }

    // Build source file path
#ifdef _WIN32
    std::replace(source_file_name.begin(), source_file_name.end(), '/', '\\');
#endif
    fs::path source_file_path = fs::path(output_dir_) / source_file_name;
    std::string source_path = source_file_path.string();

    // Check if source file exists
    if (!fs::exists(source_path)) {
      LOG_ERROR("Source file not found: %s", source_path.c_str());
      return false;
    }

    // Open patch data slice
    FileSliceStream patch_slice(
        patch_path, file_info.patch_data_offset_, file_info.patch_data_length_);

    std::string temp_output = target_path + "_tmp";

    // Apply HDiffPatch
    if (!ApplyHDiffPatchInternal(
            &source_path, patch_slice, temp_output, false)) {
      if (fs::exists(temp_output)) {
        fs::remove(temp_output);
      }
      return false;
    }

    // Verify MD5
    if (!file_info.target_md5_.empty()) {
      std::string actual_md5 = ChecksumUtils::calculate_md5_file(temp_output);
      if (actual_md5 != file_info.target_md5_) {
        LOG_ERROR("MD5 verification failed for: %s",
                  file_info.target_path_.c_str());
        fs::remove(temp_output);
        return false;
      }
    }

    // Delete old source file first if different from target
    if (source_path != target_path && fs::exists(source_path)) {
      fs::remove(source_path);
      LOG_INFO("Removed old file: %s", source_path.c_str());
    }

    // Rename temporary file to target
    if (fs::exists(target_path)) {
      fs::remove(target_path);
    }
    fs::rename(temp_output, target_path);

    return true;

  } catch (const std::exception& e) {
    LOG_ERROR("Exception in UpdateExistingFile: %s", e.what());
    std::string temp_output = target_path + "_tmp";
    std::error_code ec;
    fs::remove(temp_output, ec);
    return false;
  }
}

bool PatchDownloadService::ApplyHDiffPatchInternal(
    const std::string* source_file,
    FileSliceStream& patch_slice,
    const std::string& output_file,
    bool is_compressed) {
  try {
    // Read patch data into memory
    size_t patch_size = static_cast<size_t>(patch_slice.get_remaining_bytes());
    std::vector<uint8_t> patch_data(patch_size);

    size_t read_size = patch_slice.read(patch_data.data(), patch_data.size());

    if (read_size != patch_data.size()) {
      LOG_ERROR("Failed to read patch data completely (expected %zu, got %zu)",
                patch_data.size(),
                read_size);
      return false;
    }

    // Use HPatch utilities for applying patch
    auto result = HPatchUtils::apply_patch_from_memory(source_file,
                                                       patch_data.data(),
                                                       patch_data.size(),
                                                       output_file,
                                                       is_compressed);

    if (!result.success) {
      LOG_ERROR("Failed to apply patch: %s", result.error_message.c_str());
      return false;
    }

    return true;

  } catch (const std::exception& e) {
    LOG_ERROR("Exception in ApplyHDiffPatchInternal: %s", e.what());
    return false;
  }
}

void PatchDownloadService::MarkApplyCompleted(PipelinePatchItem& item) {
  std::lock_guard<std::mutex> lock(items_mutex_);
  item.apply_completed = true;
}

void PatchDownloadService::CleanupAppliedPatch(const PipelinePatchItem& item) {
  if (mode_ == PatchDownloadMode::kDownloadAndApply) {
    try {
      if (fs::exists(item.storage_path)) {
        fs::remove(item.storage_path);
        LOG_DEBUG("Cleaned up patch file: %s", item.patch_identifier.c_str());

        buffer_count_--;
        download_cv_.notify_all();
      }
    } catch (const std::exception& e) {
      LOG_WARN("Failed to cleanup patch file %s: %s",
               item.patch_identifier.c_str(),
               e.what());
    }
  }
}

bool PatchDownloadService::IsAllCompleted() const {
  std::lock_guard<std::mutex> lock(items_mutex_);

  for (const auto& item : patch_items_) {
    if (!item.download_completed) {
      return false;
    }

    if (mode_ == PatchDownloadMode::kDownloadAndApply &&
        !item.apply_completed) {
      return false;
    }
  }

  return true;
}

// ========== Legacy Session-based API ==========

std::string PatchDownloadService::StartPatchUpdate(
    const std::vector<PatchInfo>& patches) {
  if (!IsInitialized()) {
    LOG_ERROR("[PatchDownloadService] Service not initialized");
    return "";
  }

  auto session_id = GenerateSessionId();

  PatchSessionInfo session;
  session.start_time = std::chrono::steady_clock::now();
  session.patch_names.reserve(patches.size());

  if (patch_service_config_.create_backup) {
    for (const auto& patch : patches) {
      std::string source_path =
          patch_service_config_.manager_config.source_path + "/" +
          patch.source_file;
      if (CreateBackup(source_path)) {
        session.backup_files.push_back(source_path);
      }
    }
  }

  for (const auto& patch : patches) {
    patch_manager_->AddPatch(patch);
    session.patch_names.push_back(patch.patch_name);
  }

  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_[session_id] = std::move(session);
  }

  patch_manager_->StartAllPatches();

  LOG_INFO("[PatchDownloadService] Started session %s with %zu patches",
           session_id.c_str(),
           patches.size());
  return session_id;
}

std::string PatchDownloadService::StartPatchUpdate(const PatchInfo& patch) {
  return StartPatchUpdate(std::vector<PatchInfo>{patch});
}

bool PatchDownloadService::PausePatchUpdate(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    LOG_ERROR("[PatchDownloadService] Session not found: %s",
              session_id.c_str());
    return false;
  }

  patch_manager_->PauseAllPatches();
  LOG_INFO("[PatchDownloadService] Paused session: %s", session_id.c_str());
  return true;
}

bool PatchDownloadService::ResumePatchUpdate(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    LOG_ERROR("[PatchDownloadService] Session not found: %s",
              session_id.c_str());
    return false;
  }

  patch_manager_->ResumeAllPatches();
  LOG_INFO("[PatchDownloadService] Resumed session: %s", session_id.c_str());
  return true;
}

bool PatchDownloadService::CancelPatchUpdate(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    LOG_ERROR("[PatchDownloadService] Session not found: %s",
              session_id.c_str());
    return false;
  }

  // Rollback if configured
  if (patch_service_config_.rollback_on_failure) {
    for (const auto& backup_file : it->second.backup_files) {
      RollbackFromBackup(backup_file);
    }
  }

  // Cancel items
  for (const auto& patch_name : it->second.patch_names) {
    auto item = patch_manager_->GetItemByPatchName(patch_name);
    if (item) {
      item->TransitionTo(DownloadState::kCancelled);
    }
  }

  sessions_.erase(it);
  LOG_INFO("[PatchDownloadService] Cancelled session: %s", session_id.c_str());
  return true;
}

PatchDownloadService::PatchSessionProgress
PatchDownloadService::GetSessionProgress(const std::string& session_id) const {
  PatchSessionProgress progress;
  progress.session_id = session_id;

  std::lock_guard<std::mutex> lock(sessions_mutex_);
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    return progress;
  }

  const auto& session = it->second;
  progress.total_patches = session.patch_names.size();
  progress.downloaded_patches = session.downloaded_count;
  progress.applied_patches = session.applied_count;
  progress.failed_patches = session.failed_count;

  double total_download_progress = 0.0;
  double total_apply_progress = 0.0;

  for (const auto& patch_name : session.patch_names) {
    auto item = patch_manager_->GetItemByPatchName(patch_name);
    if (item) {
      progress.total_bytes += item->GetTotalSize();
      progress.downloaded_bytes += item->GetDownloadedSize();

      if (item->GetTotalSize() > 0) {
        total_download_progress +=
            static_cast<double>(item->GetDownloadedSize()) /
            item->GetTotalSize();
      }
      total_apply_progress += item->GetApplyProgress();
    }
  }

  if (progress.total_patches > 0) {
    progress.download_progress =
        total_download_progress / progress.total_patches * 100.0;
    progress.apply_progress =
        total_apply_progress / progress.total_patches * 100.0;
    progress.overall_progress =
        (progress.download_progress + progress.apply_progress) / 2.0;
  }

  return progress;
}

std::vector<std::string> PatchDownloadService::GetActiveSessions() const {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  std::vector<std::string> result;
  result.reserve(sessions_.size());
  for (const auto& [id, _] : sessions_) {
    result.push_back(id);
  }
  return result;
}

std::string PatchDownloadService::GenerateSessionId() {
  auto counter = session_counter_.fetch_add(1);
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch())
                       .count();

  std::ostringstream oss;
  oss << "patch_" << timestamp << "_" << counter;
  return oss.str();
}

bool PatchDownloadService::CreateBackup(const std::string& file_path) {
  std::error_code ec;
  if (!fs::exists(file_path, ec)) {
    return false;
  }

  std::string backup_path = patch_service_config_.backup_path + "/" +
                            fs::path(file_path).filename().string() + ".bak";

  fs::copy_file(
      file_path, backup_path, fs::copy_options::overwrite_existing, ec);

  if (ec) {
    LOG_WARN("[PatchDownloadService] Failed to backup %s: %s",
             file_path.c_str(),
             ec.message().c_str());
    return false;
  }

  LOG_DEBUG("[PatchDownloadService] Backed up: %s -> %s",
            file_path.c_str(),
            backup_path.c_str());
  return true;
}

bool PatchDownloadService::RollbackFromBackup(const std::string& file_path) {
  std::string backup_path = patch_service_config_.backup_path + "/" +
                            fs::path(file_path).filename().string() + ".bak";

  std::error_code ec;
  if (!fs::exists(backup_path, ec)) {
    LOG_WARN("[PatchDownloadService] Backup not found: %s",
             backup_path.c_str());
    return false;
  }

  fs::copy_file(
      backup_path, file_path, fs::copy_options::overwrite_existing, ec);

  if (ec) {
    LOG_ERROR("[PatchDownloadService] Failed to rollback %s: %s",
              file_path.c_str(),
              ec.message().c_str());
    return false;
  }

  LOG_INFO("[PatchDownloadService] Rolled back: %s", file_path.c_str());
  return true;
}

void PatchDownloadService::OnPatchApplied(const std::string& patch_name,
                                          bool success) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);

  for (auto& [session_id, session] : sessions_) {
    auto it = std::find(
        session.patch_names.begin(), session.patch_names.end(), patch_name);
    if (it != session.patch_names.end()) {
      if (success) {
        session.applied_count++;
      } else {
        session.failed_count++;
      }

      if (patch_applied_callback_) {
        patch_applied_callback_(patch_name, success);
      }

      CheckAllApplied(session_id);
      break;
    }
  }
}

void PatchDownloadService::CheckAllApplied(const std::string& session_id) {
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    return;
  }

  const auto& session = it->second;
  auto total_done = session.applied_count + session.failed_count;

  if (total_done >= session.patch_names.size()) {
    bool all_success = (session.failed_count == 0);

    LOG_INFO(
        "[PatchDownloadService] Session %s completed. Applied: %zu, Failed: "
        "%zu",
        session_id.c_str(),
        session.applied_count,
        session.failed_count);

    if (all_patches_applied_callback_) {
      all_patches_applied_callback_(all_success);
    }
  }
}

QUATON_NAMESPACE_END
