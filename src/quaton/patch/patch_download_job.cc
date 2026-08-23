#include "quaton/patch/patch_download_job.h"

#include <filesystem>
#include <fstream>

#include "quaton/http_client.h"
#include "quaton/logger.h"
#include "quaton/utils/checksum.h"
#include "quaton/utils/compression.h"

namespace fs = std::filesystem;

namespace Quaton {

PatchDownloadJob::PatchDownloadJob(const std::string& job_id,
                                   std::shared_ptr<HttpClient> http_client,
                                   const std::string& output_dir,
                                   const std::string& patch_dir)
    : DownloadJob(job_id, std::move(http_client)),
      output_dir_(output_dir),
      patch_dir_(patch_dir) {
}

void PatchDownloadJob::AddPatchItem(std::shared_ptr<PatchDownloadItem> item) {
  {
    std::lock_guard<std::mutex> lock(patch_items_mutex_);
    patch_items_[item->GetId()] = item;
  }
  // Also add to base class items
  AddItem(item);
}

std::vector<std::shared_ptr<PatchDownloadItem>>
PatchDownloadJob::GetPatchItems() const {
  std::lock_guard<std::mutex> lock(patch_items_mutex_);
  std::vector<std::shared_ptr<PatchDownloadItem>> result;
  result.reserve(patch_items_.size());
  for (const auto& [id, item] : patch_items_) {
    result.push_back(item);
  }
  return result;
}

std::shared_ptr<PatchDownloadItem> PatchDownloadJob::GetPatchItemById(
    const std::string& item_id) const {
  std::lock_guard<std::mutex> lock(patch_items_mutex_);
  auto it = patch_items_.find(item_id);
  if (it != patch_items_.end()) {
    return it->second;
  }
  return nullptr;
}

bool PatchDownloadJob::ProcessItem(std::shared_ptr<DownloadItem> item) {
  // Step 1: Download patch
  item->TransitionTo(DownloadState::kDownloading, "Downloading patch");
  if (!DownloadPatch(item)) {
    return false;
  }

  // Step 2: Verify patch (if enabled)
  if (verify_enabled_) {
    item->TransitionTo(DownloadState::kVerifying, "Verifying patch");
    if (!VerifyPatch(item)) {
      return false;
    }
  }

  // Step 3: Apply patch (if enabled)
  if (apply_enabled_) {
    item->TransitionTo(DownloadState::kApplying, "Applying patch");
    if (!ApplyPatch(item)) {
      return false;
    }
    applied_count_.fetch_add(1);
  }

  // Step 4: Cleanup (if enabled and apply was successful)
  if (cleanup_enabled_ && apply_enabled_) {
    CleanupPatch(item);
  }

  return true;
}

void PatchDownloadJob::OnAllItemsProcessed() {
  DownloadJob::OnAllItemsProcessed();
  LOG_INFO(
      "Job %s: All patches processed. Total: %zu, Completed: %zu, Failed: %zu, "
      "Applied: %zu",
      job_id_.c_str(),
      GetItemCount(),
      GetCompletedItemCount(),
      GetFailedItemCount(),
      applied_count_.load());
}

void PatchDownloadJob::OnItemCompleted(std::shared_ptr<DownloadItem> item) {
  DownloadJob::OnItemCompleted(item);
  LOG_DEBUG("Job %s: Patch %s completed successfully",
            job_id_.c_str(),
            item->GetId().c_str());
}

void PatchDownloadJob::OnItemFailed(std::shared_ptr<DownloadItem> item) {
  DownloadJob::OnItemFailed(item);
  LOG_ERROR("Job %s: Patch %s failed: %s",
            job_id_.c_str(),
            item->GetId().c_str(),
            item->GetError().error_message.c_str());

  // Try to restore from backup if we made one
  auto patch_item = std::dynamic_pointer_cast<PatchDownloadItem>(item);
  if (patch_item && backup_enabled_) {
    std::string source_path = GetSourcePath(item);
    std::lock_guard<std::mutex> lock(backup_mutex_);
    auto it = backup_paths_.find(source_path);
    if (it != backup_paths_.end()) {
      RestoreFromBackup(source_path, it->second);
      backup_paths_.erase(it);
    }
  }
}

std::string PatchDownloadJob::GetDownloadUrl(
    std::shared_ptr<DownloadItem> item) const {
  std::string url = item->GetUrl();
  if (url.empty() && !patch_base_url_.empty()) {
    // patch_base_url_ is the diff_download url_prefix which ends with '/';
    // trim it before joining to avoid a double slash that S3-compatible
    // buckets reject with HTTP 400.
    std::string base = patch_base_url_;
    while (!base.empty() && base.back() == '/') {
      base.pop_back();
    }
    url = base + "/" + item->GetId();
    if (!patch_url_suffix_.empty()) {
      url += patch_url_suffix_;
    }
  }
  return url;
}

std::string PatchDownloadJob::GetPatchPath(
    std::shared_ptr<DownloadItem> item) const {
  fs::path patch_path = fs::path(patch_dir_) / item->GetId();
  return patch_path.string();
}

std::string PatchDownloadJob::GetSourcePath(
    std::shared_ptr<DownloadItem> item) const {
  fs::path source_path = fs::path(output_dir_) / item->GetFilePath();
  return source_path.string();
}

std::string PatchDownloadJob::GetOutputPath(
    std::shared_ptr<DownloadItem> item) const {
  // For patch updates, output path is same as source path
  return GetSourcePath(item);
}

bool PatchDownloadJob::DownloadPatch(std::shared_ptr<DownloadItem> item) {
  try {
    std::string url = GetDownloadUrl(item);
    if (url.empty()) {
      item->SetError(-1, "No download URL", "", false);
      return false;
    }

    std::string patch_path = GetPatchPath(item);

    fs::path parent_path = fs::path(patch_path).parent_path();
    if (!parent_path.empty()) {
      std::error_code ec;
      fs::create_directories(parent_path, ec);
      if (ec) {
        item->SetError(-2, "Failed to create directory", parent_path.string());
        return false;
      }
    }

    // Download patch using HTTP client
    auto response = http_client_->get_async(url).get();

    if (response.empty()) {
      item->SetError(-3, "Empty response from server", url);
      return false;
    }

    // Write to file
    std::ofstream file(patch_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
      item->SetError(-4, "Failed to open patch file", patch_path);
      return false;
    }

    file.write(reinterpret_cast<const char*>(response.data()), response.size());
    file.close();

    if (!file.good()) {
      item->SetError(-5, "Failed to write patch file", patch_path);
      return false;
    }

    item->UpdateProgress(response.size(), response.size());
    patch_bytes_downloaded_.fetch_add(response.size());

    LOG_DEBUG("Job %s: Downloaded patch %s (%zu bytes)",
              job_id_.c_str(),
              item->GetId().c_str(),
              response.size());

    return true;

  } catch (const std::exception& e) {
    item->SetError(-6, "Patch download failed", e.what());
    LOG_ERROR("Job %s: Failed to download patch %s: %s",
              job_id_.c_str(),
              item->GetId().c_str(),
              e.what());
    return false;
  }
}

bool PatchDownloadJob::VerifyPatch(std::shared_ptr<DownloadItem> item) {
  std::string expected_checksum = item->GetExpectedChecksum();
  if (expected_checksum.empty()) {
    // No checksum to verify
    return true;
  }

  std::string patch_path = GetPatchPath(item);

  if (!fs::exists(patch_path)) {
    item->SetError(-10, "Patch file not found for verification", patch_path);
    return false;
  }

  std::string calculated_checksum =
      ChecksumUtils::calculate_md5_file(patch_path);

  if (calculated_checksum.empty()) {
    item->SetError(-11, "Failed to calculate patch checksum", patch_path);
    return false;
  }

  // Compare checksums (case-insensitive)
  std::string expected_lower = expected_checksum;
  std::string calculated_lower = calculated_checksum;
  std::transform(expected_lower.begin(),
                 expected_lower.end(),
                 expected_lower.begin(),
                 ::tolower);
  std::transform(calculated_lower.begin(),
                 calculated_lower.end(),
                 calculated_lower.begin(),
                 ::tolower);

  if (expected_lower != calculated_lower) {
    item->SetError(
        -12,
        "Patch checksum mismatch",
        "Expected: " + expected_checksum + ", Got: " + calculated_checksum);
    return false;
  }

  LOG_DEBUG("Job %s: Verified patch %s (MD5: %s)",
            job_id_.c_str(),
            item->GetId().c_str(),
            calculated_checksum.c_str());

  return true;
}

bool PatchDownloadJob::ApplyPatch(std::shared_ptr<DownloadItem> item) {
  auto patch_item = std::dynamic_pointer_cast<PatchDownloadItem>(item);
  if (!patch_item) {
    item->SetError(-20, "Invalid patch item type", "");
    return false;
  }

  std::string patch_path = GetPatchPath(item);
  std::string source_path = GetSourcePath(item);
  std::string output_path = GetOutputPath(item);

  // Backup original file if enabled
  if (backup_enabled_) {
    std::string backup_path = BackupFile(source_path);
    if (!backup_path.empty()) {
      std::lock_guard<std::mutex> lock(backup_mutex_);
      backup_paths_[source_path] = backup_path;
    }
  }

  // Determine patch type and apply
  PatchType patch_type = patch_item->GetPatchType();

  bool success = false;
  switch (patch_type) {
    case PatchType::kLdiff:
      success = ApplyLdiffPatch(patch_path, source_path, output_path);
      break;
    case PatchType::kHPatch:
      success = ApplyHdiffPatch(patch_path, source_path, output_path);
      break;
    case PatchType::kBsDiff:
    case PatchType::kXDelta:
    case PatchType::kUnknown:
      // For unsupported types, log error
      item->SetError(-21, "Unsupported patch type", "");
      success = false;
      break;
  }

  if (!success) {
    return false;
  }

  // Verify output file if target checksum is available
  std::string target_checksum = patch_item->GetTargetHash();
  if (!target_checksum.empty()) {
    std::string output_checksum =
        ChecksumUtils::calculate_md5_file(output_path);
    std::string target_lower = target_checksum;
    std::string output_lower = output_checksum;
    std::transform(target_lower.begin(),
                   target_lower.end(),
                   target_lower.begin(),
                   ::tolower);
    std::transform(output_lower.begin(),
                   output_lower.end(),
                   output_lower.begin(),
                   ::tolower);

    if (target_lower != output_lower) {
      item->SetError(
          -26,
          "Output checksum mismatch after patch",
          "Expected: " + target_checksum + ", Got: " + output_checksum);
      return false;
    }
  }

  LOG_DEBUG("Job %s: Applied patch %s successfully",
            job_id_.c_str(),
            item->GetId().c_str());

  return true;
}

bool PatchDownloadJob::ApplyLdiffPatch(const std::string& patch_path,
                                       const std::string& source_path,
                                       const std::string& output_path) {
  // LDIFF patch application
  // This would use the actual LDIFF library
  // For now, placeholder implementation

  try {
    // Check if source file exists (for non-new files)
    bool source_exists = fs::exists(source_path);

    // Read patch data
    std::ifstream patch_file(patch_path, std::ios::binary);
    if (!patch_file.is_open()) {
      LOG_ERROR("Failed to open patch file: %s", patch_path.c_str());
      return false;
    }

    std::vector<uint8_t> patch_data(
        (std::istreambuf_iterator<char>(patch_file)),
        std::istreambuf_iterator<char>());
    patch_file.close();

    // Check if patch is compressed (zstd)
    bool is_compressed = patch_data.size() >= 4 && patch_data[0] == 0x28 &&
                         patch_data[1] == 0xB5 && patch_data[2] == 0x2F &&
                         patch_data[3] == 0xFD;

    if (is_compressed) {
      patch_data = CompressionUtils::DecompressZstd(patch_data);
      if (patch_data.empty()) {
        LOG_ERROR("Failed to decompress patch: %s", patch_path.c_str());
        return false;
      }
    }

    // TODO: Actual LDIFF patch application
    // This requires integration with the ldiff library
    // For now, we'll log that this needs implementation

    LOG_WARN("LDIFF patch application not fully implemented: %s -> %s",
             patch_path.c_str(),
             output_path.c_str());

    // Placeholder: if source doesn't exist, treat patch as full file
    if (!source_exists) {
      std::ofstream output_file(output_path,
                                std::ios::binary | std::ios::trunc);
      if (!output_file.is_open()) {
        return false;
      }
      output_file.write(reinterpret_cast<const char*>(patch_data.data()),
                        patch_data.size());
      output_file.close();
      return output_file.good();
    }

    return true;

  } catch (const std::exception& e) {
    LOG_ERROR("LDIFF patch failed: %s", e.what());
    return false;
  }
}

bool PatchDownloadJob::ApplyHdiffPatch(const std::string& patch_path,
                                       const std::string& source_path,
                                       const std::string& output_path) {
  // HDiff patch application
  // This would use the HDiffPatch library
  // For now, placeholder implementation

  try {
    LOG_WARN("HDiff patch application not fully implemented: %s -> %s",
             patch_path.c_str(),
             output_path.c_str());

    // TODO: Actual HDiff patch application
    // This requires integration with the HDiffPatch library

    return true;

  } catch (const std::exception& e) {
    LOG_ERROR("HDiff patch failed: %s", e.what());
    return false;
  }
}

std::string PatchDownloadJob::BackupFile(const std::string& file_path) {
  if (!fs::exists(file_path)) {
    return "";
  }

  try {
    std::string backup_path = file_path + ".bak";
    fs::copy_file(file_path, backup_path, fs::copy_options::overwrite_existing);
    LOG_DEBUG("Job %s: Backed up %s to %s",
              job_id_.c_str(),
              file_path.c_str(),
              backup_path.c_str());
    return backup_path;
  } catch (const std::exception& e) {
    LOG_WARN("Job %s: Failed to backup %s: %s",
             job_id_.c_str(),
             file_path.c_str(),
             e.what());
    return "";
  }
}

bool PatchDownloadJob::RestoreFromBackup(const std::string& original_path,
                                         const std::string& backup_path) {
  try {
    if (fs::exists(backup_path)) {
      fs::copy_file(
          backup_path, original_path, fs::copy_options::overwrite_existing);
      fs::remove(backup_path);
      LOG_INFO("Job %s: Restored %s from backup",
               job_id_.c_str(),
               original_path.c_str());
      return true;
    }
  } catch (const std::exception& e) {
    LOG_ERROR("Job %s: Failed to restore %s from backup: %s",
              job_id_.c_str(),
              original_path.c_str(),
              e.what());
  }
  return false;
}

bool PatchDownloadJob::CleanupPatch(std::shared_ptr<DownloadItem> item) {
  std::string patch_path = GetPatchPath(item);

  std::error_code ec;
  if (fs::remove(patch_path, ec)) {
    LOG_DEBUG(
        "Job %s: Cleaned up patch %s", job_id_.c_str(), item->GetId().c_str());
    return true;
  }

  if (ec) {
    LOG_WARN("Job %s: Failed to cleanup patch %s: %s",
             job_id_.c_str(),
             item->GetId().c_str(),
             ec.message().c_str());
  }

  return false;
}

}  // namespace Quaton
