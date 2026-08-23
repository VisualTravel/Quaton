#include "quaton/chunk/chunk_download_job.h"

#include <filesystem>
#include <fstream>

#include "quaton/http_client.h"
#include "quaton/logger.h"
#include "quaton/utils/checksum.h"
#include "quaton/utils/compression.h"

namespace fs = std::filesystem;

namespace Quaton {

ChunkDownloadJob::ChunkDownloadJob(const std::string& job_id,
                                   std::shared_ptr<HttpClient> http_client,
                                   const std::string& output_dir)
    : DownloadJob(job_id, std::move(http_client)), output_dir_(output_dir) {
}

void ChunkDownloadJob::AddChunkItem(std::shared_ptr<ChunkDownloadItem> item) {
  {
    std::lock_guard<std::mutex> lock(chunk_items_mutex_);
    chunk_items_[item->GetId()] = item;
  }
  // Also add to base class items
  AddItem(item);
}

std::vector<std::shared_ptr<ChunkDownloadItem>>
ChunkDownloadJob::GetChunkItems() const {
  std::lock_guard<std::mutex> lock(chunk_items_mutex_);
  std::vector<std::shared_ptr<ChunkDownloadItem>> result;
  result.reserve(chunk_items_.size());
  for (const auto& [id, item] : chunk_items_) {
    result.push_back(item);
  }
  return result;
}

std::shared_ptr<ChunkDownloadItem> ChunkDownloadJob::GetChunkItemById(
    const std::string& item_id) const {
  std::lock_guard<std::mutex> lock(chunk_items_mutex_);
  auto it = chunk_items_.find(item_id);
  if (it != chunk_items_.end()) {
    return it->second;
  }
  return nullptr;
}

bool ChunkDownloadJob::ProcessItem(std::shared_ptr<DownloadItem> item) {
  // Transition to downloading
  item->TransitionTo(DownloadState::kDownloading, "Starting chunk download");

  // Step 1: Download
  if (!DownloadFile(item)) {
    return false;
  }

  // Step 2: Decompress (if enabled and needed)
  if (decompress_enabled_) {
    if (!DecompressChunk(item)) {
      return false;
    }
  }

  // Step 3: Verify (if enabled)
  if (verify_enabled_) {
    item->TransitionTo(DownloadState::kVerifying, "Verifying chunk");
    if (!VerifyFile(item)) {
      return false;
    }
  }

  // Step 4: Save to database (if enabled)
  if (database_enabled_) {
    if (!SaveToDatabase(item)) {
      LOG_WARN("Job %s: Failed to save item %s to database",
               job_id_.c_str(),
               item->GetId().c_str());
      // Don't fail the item for database errors
    }
  }

  return true;
}

void ChunkDownloadJob::OnAllItemsProcessed() {
  DownloadJob::OnAllItemsProcessed();
  LOG_INFO(
      "Job %s: All chunk downloads completed. Total: %zu, Completed: %zu, "
      "Failed: %zu",
      job_id_.c_str(),
      GetItemCount(),
      GetCompletedItemCount(),
      GetFailedItemCount());
}

void ChunkDownloadJob::OnItemCompleted(std::shared_ptr<DownloadItem> item) {
  DownloadJob::OnItemCompleted(item);
  LOG_DEBUG("Job %s: Chunk %s completed successfully",
            job_id_.c_str(),
            item->GetId().c_str());
}

void ChunkDownloadJob::OnItemFailed(std::shared_ptr<DownloadItem> item) {
  DownloadJob::OnItemFailed(item);
  LOG_ERROR("Job %s: Chunk %s failed: %s",
            job_id_.c_str(),
            item->GetId().c_str(),
            item->GetError().error_message.c_str());
}

std::string ChunkDownloadJob::GetDownloadUrl(
    std::shared_ptr<DownloadItem> item) const {
  std::string url = item->GetUrl();
  if (url.empty() && !chunk_base_url_.empty()) {
    url = chunk_base_url_ + "/" + item->GetId();
  }
  return url;
}

std::string ChunkDownloadJob::GetOutputPath(
    std::shared_ptr<DownloadItem> item) const {
  fs::path output_path = fs::path(output_dir_) / item->GetFilePath();
  return output_path.string();
}

bool ChunkDownloadJob::DownloadFile(std::shared_ptr<DownloadItem> item) {
  try {
    std::string url = GetDownloadUrl(item);
    if (url.empty()) {
      item->SetError(-1, "No download URL", "", false);
      return false;
    }

    std::string output_path = GetOutputPath(item);

    fs::path parent_path = fs::path(output_path).parent_path();
    if (!parent_path.empty()) {
      std::error_code ec;
      fs::create_directories(parent_path, ec);
      if (ec) {
        item->SetError(-2, "Failed to create directory", parent_path.string());
        return false;
      }
    }

    // Download file using HTTP client
    auto response = http_client_->get_async(url).get();

    if (response.empty()) {
      item->SetError(-3, "Empty response from server", url);
      return false;
    }

    // Write to file
    std::ofstream file(output_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
      item->SetError(-4, "Failed to open output file", output_path);
      return false;
    }

    file.write(reinterpret_cast<const char*>(response.data()), response.size());
    file.close();

    if (!file.good()) {
      item->SetError(-5, "Failed to write file", output_path);
      return false;
    }

    item->UpdateProgress(response.size(), response.size());

    LOG_DEBUG("Job %s: Downloaded chunk %s (%zu bytes)",
              job_id_.c_str(),
              item->GetId().c_str(),
              response.size());

    return true;

  } catch (const std::exception& e) {
    item->SetError(-6, "Download failed", e.what());
    LOG_ERROR("Job %s: Failed to download chunk %s: %s",
              job_id_.c_str(),
              item->GetId().c_str(),
              e.what());
    return false;
  }
}

bool ChunkDownloadJob::DecompressChunk(std::shared_ptr<DownloadItem> item) {
  // Cast to ChunkDownloadItem to check compression
  auto chunk_item = std::dynamic_pointer_cast<ChunkDownloadItem>(item);
  if (!chunk_item) {
    // Not a chunk item, skip decompression
    return true;
  }

  // Check if decompression is needed based on item properties
  // For now, assume files ending with .zst need decompression
  std::string output_path = GetOutputPath(item);
  if (output_path.size() < 4 ||
      output_path.substr(output_path.size() - 4) != ".zst") {
    // No decompression needed
    return true;
  }

  try {
    std::string decompressed_path =
        output_path.substr(0, output_path.size() - 4);

    // Read compressed data
    std::ifstream compressed_file(output_path, std::ios::binary);
    if (!compressed_file.is_open()) {
      item->SetError(-10, "Failed to open compressed file", output_path);
      return false;
    }

    std::vector<uint8_t> compressed_data(
        (std::istreambuf_iterator<char>(compressed_file)),
        std::istreambuf_iterator<char>());
    compressed_file.close();

    // Decompress using zstd
    std::vector<uint8_t> decompressed_data =
        CompressionUtils::DecompressZstd(compressed_data);

    if (decompressed_data.empty()) {
      item->SetError(-11, "Decompression failed", output_path);
      return false;
    }

    // Write decompressed data
    std::ofstream decompressed_file(decompressed_path,
                                    std::ios::binary | std::ios::trunc);
    if (!decompressed_file.is_open()) {
      item->SetError(-12, "Failed to open output file", decompressed_path);
      return false;
    }

    decompressed_file.write(
        reinterpret_cast<const char*>(decompressed_data.data()),
        decompressed_data.size());
    decompressed_file.close();

    // Remove compressed file
    std::error_code ec;
    fs::remove(output_path, ec);

    LOG_DEBUG("Job %s: Decompressed chunk %s (%zu -> %zu bytes)",
              job_id_.c_str(),
              item->GetId().c_str(),
              compressed_data.size(),
              decompressed_data.size());

    return true;

  } catch (const std::exception& e) {
    item->SetError(-13, "Decompression error", e.what());
    return false;
  }
}

bool ChunkDownloadJob::VerifyFile(std::shared_ptr<DownloadItem> item) {
  std::string expected_checksum = item->GetExpectedChecksum();
  if (expected_checksum.empty()) {
    // No checksum to verify
    return true;
  }

  std::string output_path = GetOutputPath(item);

  // Handle decompressed file path
  if (output_path.size() >= 4 &&
      output_path.substr(output_path.size() - 4) == ".zst") {
    output_path = output_path.substr(0, output_path.size() - 4);
  }

  if (!fs::exists(output_path)) {
    item->SetError(-20, "File not found for verification", output_path);
    return false;
  }

  std::string calculated_checksum =
      ChecksumUtils::calculate_md5_file(output_path);

  if (calculated_checksum.empty()) {
    item->SetError(-21, "Failed to calculate checksum", output_path);
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
        -22,
        "Checksum mismatch",
        "Expected: " + expected_checksum + ", Got: " + calculated_checksum);
    return false;
  }

  LOG_DEBUG("Job %s: Verified chunk %s (MD5: %s)",
            job_id_.c_str(),
            item->GetId().c_str(),
            calculated_checksum.c_str());

  return true;
}

bool ChunkDownloadJob::SaveToDatabase(std::shared_ptr<DownloadItem> item) {
  // TODO: Implement database saving
  // This would typically involve:
  // 1. Getting database connection from manager/service
  // 2. Inserting/updating download record
  // For now, just return true
  LOG_DEBUG("Job %s: Database save for %s (not implemented)",
            job_id_.c_str(),
            item->GetId().c_str());
  return true;
}

}  // namespace Quaton
