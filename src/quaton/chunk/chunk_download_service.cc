#include "quaton/chunk/chunk_download_service.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

#include "quaton/configuration/download_config.h"
#include "quaton/database/database_manager.h"
#include "quaton/http_client.h"
#include "quaton/logger.h"
#include "quaton/manifest/manifest_processor.h"
#include "quaton/resource.h"
#include "quaton/utils/checksum.h"
#include "quaton/utils/path_helper.h"
#include "quaton/utils/retry_helper.h"

#ifdef _WIN32
#include <conio.h>
#else
#include <sys/select.h>
#include <sys/time.h>
#endif

namespace fs = std::filesystem;

// Anonymous namespace to avoid name clashes with same-named functions
// in chunk_downloader.cc
namespace {
void ChunkServiceExitTrigger(std::shared_ptr<std::atomic<bool>> cancel_flag,
                             const std::atomic<bool>& stop) {
  while (!*cancel_flag && !stop.load()) {
#ifdef _WIN32
    if (_kbhit()) {
      int ch = _getch();
      if (ch == 'c' || ch == 'C') {
        *cancel_flag = true;
        LOG_INFO("Download cancelled by user");
        break;
      } else if (ch == 'r' || ch == 'R') {
        LOG_INFO("Download restart requested");
        *cancel_flag = true;
        break;
      }
    }
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

// Keyboard-exit listener thread; joins cleanly on destruction via stop flag.
// Never sets cancel_flag itself, so it cannot pollute success return values.
class ExitListenerThread {
 public:
  explicit ExitListenerThread(std::shared_ptr<std::atomic<bool>> cancel_flag)
      : thread_([cancel_flag, &stop = stop_]() {
          ChunkServiceExitTrigger(cancel_flag, stop);
        }) {}
  ~ExitListenerThread() {
    stop_.store(true);
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  ExitListenerThread(const ExitListenerThread&) = delete;
  ExitListenerThread& operator=(const ExitListenerThread&) = delete;

 private:
  std::atomic<bool> stop_{false};
  std::thread thread_;
};
}  // namespace

QUATON_NAMESPACE_BEGIN

ChunkDownloadService::ChunkDownloadService() = default;

ChunkDownloadService::~ChunkDownloadService() {
  Shutdown();
}

bool ChunkDownloadService::Initialize(const ChunkServiceConfig& config) {
  chunk_service_config_ = config;

  config_ = config;

  if (!DownloadService::Initialize()) {
    LOG_ERROR("[ChunkDownloadService] Base initialization failed");
    return false;
  }

  http_client_ = std::make_shared<HttpClient>();

  chunk_manager_ = std::make_shared<ChunkDownloadManager>(http_client_);

  if (!chunk_manager_->Initialize(config.manager_config)) {
    LOG_ERROR("[ChunkDownloadService] Manager initialization failed");
    return false;
  }

  LOG_INFO("[ChunkDownloadService] Initialized successfully");
  return true;
}

void ChunkDownloadService::SetProgressCallback(ProgressCallback callback) {
  progress_callback_ = std::move(callback);
  if (progress_tracker_) {
    progress_tracker_->set_callback(progress_callback_);
  }
}

// ============================================================================
// Download Operations (URL Mode)
// ============================================================================

std::future<int> ChunkDownloadService::Download(
    const std::string& previous_manifest_url,
    const std::string& current_manifest_url,
    const std::string& chunk_base_url,
    const std::string& output_directory,
    const std::string& filter_criteria) {
  return std::async(
      std::launch::async,
      [this,
       previous_manifest_url,
       current_manifest_url,
       chunk_base_url,
       output_directory,
       filter_criteria]() -> int {
        try {
          int thread_count = chunk_service_config_.thread_count;
          if (thread_count <= 0) {
            thread_count =
                static_cast<int>(std::thread::hardware_concurrency());
            if (thread_count == 0) thread_count = 4;
          }

          LOG_INFO("Starting download with %d threads", thread_count);

          if (!fs::exists(output_directory)) {
            fs::create_directories(output_directory);
          }

          // Split a manifest file URL into its directory base and file name;
          // a raw URL carries no integrity metadata, so checksum/size are
          // placeholders (enumerate_resources only needs base_url+id).
          auto split_manifest_url = [](const std::string& url) {
            auto slash = url.find_last_of('/');
            if (slash == std::string::npos || slash + 1 >= url.size()) {
              throw std::invalid_argument("Invalid manifest URL: " + url);
            }
            return std::make_pair(url.substr(0, slash), url.substr(slash + 1));
          };

          ChunkManifestPair manifest_pair_from;
          ChunkManifestPair manifest_pair_to;

          auto [cur_base, cur_id] = split_manifest_url(current_manifest_url);
          manifest_pair_to.set_manifest_metadata(ManifestMetadata(
              cur_base, "url", cur_id, /*use_compression=*/true, 1, 0));
          manifest_pair_to.get_chunk_info().set_base_url(chunk_base_url);

          if (!previous_manifest_url.empty()) {
            auto [prev_base, prev_id] =
                split_manifest_url(previous_manifest_url);
            manifest_pair_from.set_manifest_metadata(ManifestMetadata(
                prev_base, "url", prev_id, /*use_compression=*/true, 1, 0));
            manifest_pair_from.get_chunk_info().set_base_url(chunk_base_url);
          }

          std::string manifest_output_dir =
              chunk_service_config_.manifest_output_dir;
          if (manifest_output_dir.empty()) {
            manifest_output_dir = PathHelper::GetManifestDir();
          }

          auto [resources, update_size] =
              Resource::retrieve_resources_from_manifests(http_client_,
                                                          manifest_pair_from,
                                                          manifest_pair_to,
                                                          filter_criteria,
                                                          manifest_output_dir);

          LOG_INFO("Found %zu resources to download, total size: %llu bytes",
                   resources.size(),
                   update_size);

          SaveManifestAndConfigRecords(
              manifest_pair_to, filter_criteria, output_directory);

          if (resources.empty()) {
            LOG_INFO("No assets to download");
            return 0;
          }

          auto cancel_flag = std::make_shared<std::atomic<bool>>(false);

          progress_tracker_ = std::make_unique<ProgressTracker>(
              static_cast<int>(resources.size()),
              OperationMode::kChunkDownload);
          if (progress_callback_) {
            progress_tracker_->set_callback(progress_callback_);
          }

          DownloadContext context;
          context.output_directory = output_directory;
          context.cancel_flag = cancel_flag;
          context.completed_downloads =
              std::make_shared<std::atomic<size_t>>(0);
          context.total_downloads =
              std::make_shared<std::atomic<size_t>>(resources.size());
          context.skipped_downloads = std::make_shared<std::atomic<size_t>>(0);
          context.console_mutex = std::make_shared<std::mutex>();
          context.directory_mutex = std::make_shared<std::mutex>();
          context.progress_tracker = progress_tracker_.get();

          TaskQueue download_queue(thread_count);
          context.download_queue = &download_queue;

          ExitListenerThread exit_listener(context.cancel_flag);

          for (const auto& asset : resources) {
            download_queue.Enqueue(
                [this, asset, &context, manifest_pair_to, filter_criteria]() {
                  ProcessDownloadTask(
                      asset, context, manifest_pair_to, filter_criteria);
                });
          }

          WaitForDownloadCompletion(download_queue, context);

          download_queue.Stop();

          {
            std::lock_guard<std::mutex> lock(*context.console_mutex);
            LOG_INFO("============================================");
            LOG_INFO("Download completed!");
            LOG_INFO("Total files processed: %zu/%zu",
                     context.completed_downloads->load(),
                     context.total_downloads->load());
            LOG_INFO("Skipped (cached): %zu",
                     context.skipped_downloads->load());
            LOG_INFO("Downloaded: %zu",
                     (context.completed_downloads->load() -
                      context.skipped_downloads->load()));
            LOG_INFO("============================================");
          }

          return *cancel_flag ? -1 : 0;
        } catch (const std::exception& e) {
          LOG_ERROR("Error in ChunkDownloadService::Download: %s", e.what());
          return -1;
        }
      });
}

std::future<int> ChunkDownloadService::Download(
    const ChunkManifestPair& manifest_pair,
    const std::string& package,
    const std::string& output_directory) {
  return std::async(
      std::launch::async,
      [this, manifest_pair, package, output_directory]() -> int {
        try {
          int thread_count = chunk_service_config_.thread_count;
          if (thread_count <= 0) {
            thread_count =
                static_cast<int>(std::thread::hardware_concurrency());
            if (thread_count == 0) thread_count = 4;
          }

          LOG_INFO("Starting download with %d threads", thread_count);

          if (!fs::exists(output_directory)) {
            fs::create_directories(output_directory);
          }

          std::string manifest_output_dir =
              chunk_service_config_.manifest_output_dir;
          if (manifest_output_dir.empty()) {
            manifest_output_dir = PathHelper::GetManifestDir();
          }

          auto [resources, update_size] =
              Resource::retrieve_resources_from_manifests(http_client_,
                                                          manifest_pair,
                                                          manifest_pair,
                                                          package,
                                                          manifest_output_dir);

          LOG_INFO("Found %zu resources to download, total size: %llu bytes",
                   resources.size(),
                   update_size);

          SaveManifestAndConfigRecords(
              manifest_pair, package, output_directory);

          if (resources.empty()) {
            LOG_INFO("No assets to download");
            return 0;
          }

          progress_tracker_ = std::make_unique<ProgressTracker>(
              static_cast<int>(resources.size()),
              OperationMode::kChunkDownload);
          if (progress_callback_) {
            progress_tracker_->set_callback(progress_callback_);
          }

          auto cancel_flag = std::make_shared<std::atomic<bool>>(false);

          DownloadContext context;
          context.output_directory = output_directory;
          context.cancel_flag = cancel_flag;
          context.completed_downloads =
              std::make_shared<std::atomic<size_t>>(0);
          context.total_downloads =
              std::make_shared<std::atomic<size_t>>(resources.size());
          context.skipped_downloads = std::make_shared<std::atomic<size_t>>(0);
          context.console_mutex = std::make_shared<std::mutex>();
          context.directory_mutex = std::make_shared<std::mutex>();
          context.progress_tracker = progress_tracker_.get();

          TaskQueue download_queue(thread_count);
          context.download_queue = &download_queue;

          ExitListenerThread exit_listener(context.cancel_flag);

          for (const auto& asset : resources) {
            download_queue.Enqueue(
                [this, asset, &context, manifest_pair, package]() {
                  ProcessDownloadTask(asset, context, manifest_pair, package);
                });
          }

          WaitForDownloadCompletion(download_queue, context);

          download_queue.Stop();

          {
            std::lock_guard<std::mutex> lock(*context.console_mutex);
            LOG_INFO("============================================");
            LOG_INFO("Download completed!");
            LOG_INFO("Total files processed: %zu/%zu",
                     context.completed_downloads->load(),
                     context.total_downloads->load());
            LOG_INFO("Skipped (cached): %zu",
                     context.skipped_downloads->load());
            LOG_INFO("Downloaded: %zu",
                     (context.completed_downloads->load() -
                      context.skipped_downloads->load()));
            LOG_INFO("============================================");
          }

          return *cancel_flag ? -1 : 0;
        } catch (const std::exception& e) {
          LOG_ERROR("Error in ChunkDownloadService::Download: %s", e.what());
          return -1;
        }
      });
}

std::future<int> ChunkDownloadService::Restore(
    const ChunkManifestPair& manifest_pair,
    const std::string& output_directory) {
  return std::async(
      std::launch::async, [this, manifest_pair, output_directory]() -> int {
        try {
          LOG_INFO("Starting repair process...");

          if (!fs::exists(output_directory)) {
            LOG_ERROR("Output directory does not exist: %s",
                      output_directory.c_str());
            return -1;
          }

          int thread_count = chunk_service_config_.thread_count;
          if (thread_count <= 0) {
            thread_count =
                static_cast<int>(std::thread::hardware_concurrency());
            if (thread_count == 0) thread_count = 4;
          }

          std::string manifest_output_dir =
              chunk_service_config_.manifest_output_dir;
          if (manifest_output_dir.empty()) {
            manifest_output_dir = PathHelper::GetManifestDir();
          }

          auto [resources, update_size] =
              Resource::retrieve_resources_from_manifests(http_client_,
                                                          manifest_pair,
                                                          manifest_pair,
                                                          "game",
                                                          manifest_output_dir);

          LOG_INFO("Found %zu resources in manifest", resources.size());

          if (resources.empty()) {
            LOG_INFO("No assets found in manifest");
            return 0;
          }

          auto& db_manager = DatabaseManager::Instance();

          std::vector<std::shared_ptr<Resource>> assets_to_repair;
          std::mutex console_mutex;

          for (const auto& asset : resources) {
            std::string asset_name_fixed = asset->name_;
#ifdef _WIN32
            std::replace(
                asset_name_fixed.begin(), asset_name_fixed.end(), '/', '\\');
#endif
            fs::path output_path =
                fs::path(output_directory) / asset_name_fixed;
            std::string output_path_str = output_path.string();

            std::string final_package_id = "game";
            std::string final_build_id =
                !manifest_pair.get_api_build_id().empty()
                    ? manifest_pair.get_api_build_id()
                    : manifest_pair.get_manifest_metadata().get_id();
            std::string final_version = !manifest_pair.get_api_tag().empty()
                                            ? manifest_pair.get_api_tag()
                                            : final_build_id;
            std::string final_depot_id =
                !manifest_pair.get_api_category_id().empty()
                    ? manifest_pair.get_api_category_id()
                    : (final_package_id + "_depot");

            FileValidationParams validation_params;
            validation_params.file_path = asset->name_;
            validation_params.file_checksum = asset->hash_;
            validation_params.install_dir = output_directory;
            validation_params.package_id = final_package_id;
            validation_params.branch = "main";
            validation_params.build_id = final_build_id;
            validation_params.depot_id = final_depot_id;
            validation_params.file_ver = final_version;

            if (!db_manager.IsFileValid(validation_params)) {
              assets_to_repair.push_back(asset);
              std::lock_guard<std::mutex> lock(console_mutex);
              if (!fs::exists(output_path_str)) {
                LOG_WARN("Missing file detected: %s", asset->name_.c_str());
              } else {
                LOG_WARN("Corrupted file detected: %s", asset->name_.c_str());
              }
            }
          }

          if (assets_to_repair.empty()) {
            LOG_INFO("No files need repair");
            return 0;
          }

          LOG_INFO("Found %zu files to repair", assets_to_repair.size());

          progress_tracker_ = std::make_unique<ProgressTracker>(
              static_cast<int>(assets_to_repair.size()),
              OperationMode::kChunkDownload);
          if (progress_callback_) {
            progress_tracker_->set_callback(progress_callback_);
          }

          DownloadContext context;
          context.output_directory = output_directory;
          context.cancel_flag = std::make_shared<std::atomic<bool>>(false);
          context.completed_downloads =
              std::make_shared<std::atomic<size_t>>(0);
          context.total_downloads =
              std::make_shared<std::atomic<size_t>>(assets_to_repair.size());
          context.console_mutex = std::make_shared<std::mutex>();
          context.directory_mutex = std::make_shared<std::mutex>();
          context.skipped_downloads = std::make_shared<std::atomic<size_t>>(0);
          context.progress_tracker = progress_tracker_.get();

          TaskQueue download_queue(thread_count);
          context.download_queue = &download_queue;

          ExitListenerThread exit_listener(context.cancel_flag);

          for (const auto& asset : assets_to_repair) {
            download_queue.Enqueue([this, asset, &context, manifest_pair]() {
              ProcessDownloadTask(asset, context, manifest_pair, "game");
            });
          }

          WaitForDownloadCompletion(download_queue, context);
          download_queue.Stop();

          {
            std::lock_guard<std::mutex> lock(*context.console_mutex);
            LOG_INFO("============================================");
            LOG_INFO("Repair completed!");
            LOG_INFO("Total files processed: %zu/%zu",
                     context.completed_downloads->load(),
                     context.total_downloads->load());
            LOG_INFO("============================================");
          }

          return *context.cancel_flag ? -1 : 0;

        } catch (const std::exception& e) {
          LOG_ERROR("Error in ChunkDownloadService::Restore: %s", e.what());
          return -1;
        }
      });
}

// ============================================================================
// Download Operations (Resource Mode)
// ============================================================================

std::string ChunkDownloadService::StartDownload(
    const std::vector<Resource>& resources) {
  if (!IsInitialized()) {
    LOG_ERROR("[ChunkDownloadService] Service not initialized");
    return "";
  }

  auto session_id = GenerateSessionId();

  SessionInfo session;
  session.start_time = std::chrono::steady_clock::now();
  session.file_names.reserve(resources.size());

  for (const auto& resource : resources) {
    chunk_manager_->AddResource(resource);
    session.file_names.push_back(resource.name_);
  }

  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_[session_id] = std::move(session);
  }

  chunk_manager_->StartAllDownloads();

  LOG_INFO("[ChunkDownloadService] Started session {} with {} files",
           session_id,
           resources.size());
  return session_id;
}

std::string ChunkDownloadService::StartDownload(const Resource& resource) {
  return StartDownload(std::vector<Resource>{resource});
}

bool ChunkDownloadService::PauseDownload(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    LOG_ERROR("[ChunkDownloadService] Session not found: {}", session_id);
    return false;
  }

  chunk_manager_->PauseAllDownloads();
  LOG_INFO("[ChunkDownloadService] Paused session: {}", session_id);
  return true;
}

bool ChunkDownloadService::ResumeDownload(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    LOG_ERROR("[ChunkDownloadService] Session not found: {}", session_id);
    return false;
  }

  chunk_manager_->ResumeAllDownloads();
  LOG_INFO("[ChunkDownloadService] Resumed session: {}", session_id);
  return true;
}

bool ChunkDownloadService::CancelDownload(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    LOG_ERROR("[ChunkDownloadService] Session not found: {}", session_id);
    return false;
  }

  for (const auto& file_name : it->second.file_names) {
    auto item = chunk_manager_->GetItemByFileName(file_name);
    if (item) {
      item->TransitionTo(DownloadState::kCancelled);
    }
  }

  sessions_.erase(it);
  LOG_INFO("[ChunkDownloadService] Cancelled session: {}", session_id);
  return true;
}

ChunkDownloadService::SessionProgress ChunkDownloadService::GetSessionProgress(
    const std::string& session_id) const {
  SessionProgress progress;
  progress.session_id = session_id;

  std::lock_guard<std::mutex> lock(sessions_mutex_);
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    return progress;
  }

  const auto& session = it->second;
  progress.total_files = session.file_names.size();
  progress.completed_files = session.completed_count;
  progress.failed_files = session.failed_count;

  for (const auto& file_name : session.file_names) {
    auto item = chunk_manager_->GetItemByFileName(file_name);
    if (item) {
      progress.total_bytes += item->GetTotalSize();
      progress.downloaded_bytes += item->GetDownloadedSize();

      const auto& item_progress = item->GetProgress();
      progress.download_speed += item_progress.speed_bytes_per_sec;
    }
  }

  if (progress.total_bytes > 0) {
    progress.progress_percentage =
        static_cast<double>(progress.downloaded_bytes) / progress.total_bytes *
        100.0;
  }

  if (progress.download_speed > 0) {
    auto remaining_bytes = progress.total_bytes - progress.downloaded_bytes;
    auto remaining_seconds =
        static_cast<int64_t>(remaining_bytes / progress.download_speed);
    progress.estimated_time_remaining = std::chrono::seconds(remaining_seconds);
  }

  return progress;
}

std::vector<std::string> ChunkDownloadService::GetActiveSessions() const {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  std::vector<std::string> result;
  result.reserve(sessions_.size());
  for (const auto& [session_id, _] : sessions_) {
    result.push_back(session_id);
  }
  return result;
}

// ============================================================================
// Private Implementation
// ============================================================================

std::string ChunkDownloadService::GenerateSessionId() {
  auto counter = session_counter_.fetch_add(1);
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch())
                       .count();

  std::ostringstream oss;
  oss << "chunk_" << timestamp << "_" << counter;
  return oss.str();
}

void ChunkDownloadService::OnFileCompleted(const std::string& file_name,
                                           bool success) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  for (auto& [session_id, session] : sessions_) {
    auto it = std::find(
        session.file_names.begin(), session.file_names.end(), file_name);
    if (it != session.file_names.end()) {
      if (success) {
        session.completed_count++;
      } else {
        session.failed_count++;
      }

      if (file_completed_callback_) {
        file_completed_callback_(file_name, success);
      }

      CheckAllCompleted(session_id);
      break;
    }
  }
}

void ChunkDownloadService::CheckAllCompleted(const std::string& session_id) {
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    return;
  }

  const auto& session = it->second;
  auto total_done = session.completed_count + session.failed_count;

  if (total_done >= session.file_names.size()) {
    bool all_success = (session.failed_count == 0);

    LOG_INFO(
        "[ChunkDownloadService] Session {} completed. Success: {}, Failed: {}",
        session_id,
        session.completed_count,
        session.failed_count);

    if (all_completed_callback_) {
      all_completed_callback_(all_success);
    }
  }
}

void ChunkDownloadService::ProcessDownloadTask(
    const std::shared_ptr<Resource>& asset,
    DownloadContext& context,
    const ChunkManifestPair& manifest_pair,
    const std::string& package) {
  if (*context.cancel_flag) return;

  if (context.progress_tracker) {
    context.progress_tracker->update_current_file(
        asset->name_, FileProcessStage::kDownloadInitiated);
  }

  try {
    auto task_http_client = std::make_shared<HttpClient>();

    std::string asset_name_fixed = asset->name_;
#ifdef _WIN32
    std::replace(asset_name_fixed.begin(), asset_name_fixed.end(), '/', '\\');
#endif
    fs::path output_path =
        fs::path(context.output_directory) / asset_name_fixed;
    std::string output_path_str = output_path.string();

    auto& db_manager = DatabaseManager::Instance();

    std::string final_package_id = manifest_pair.get_api_package_id();
    if (final_package_id.empty()) {
      final_package_id = package.empty() ? "game" : package;
    }
    std::string final_build_id =
        !manifest_pair.get_api_build_id().empty()
            ? manifest_pair.get_api_build_id()
            : manifest_pair.get_manifest_metadata().get_id();
    std::string final_version = !manifest_pair.get_api_tag().empty()
                                    ? manifest_pair.get_api_tag()
                                    : final_build_id;
    std::string final_depot_id = !manifest_pair.get_api_category_id().empty()
                                     ? manifest_pair.get_api_category_id()
                                     : (final_package_id + "_depot");

    FileValidationParams validation_params;
    validation_params.file_path = asset->name_;
    validation_params.file_checksum = asset->hash_;
    validation_params.install_dir = context.output_directory;
    validation_params.package_id = final_package_id;
    validation_params.branch = "main";
    validation_params.build_id = final_build_id;
    validation_params.depot_id = final_depot_id;
    validation_params.file_ver = final_version;

    if (db_manager.IsFileValid(validation_params)) {
      {
        std::lock_guard<std::mutex> lock(*context.console_mutex);
        (*context.skipped_downloads)++;
        (*context.completed_downloads)++;
      }

      if (context.progress_tracker) {
        context.progress_tracker->update_current_file(
            asset->name_, FileProcessStage::kVerified);
        context.progress_tracker->increment_completed();
      }

      return;
    }

    {
      std::lock_guard<std::mutex> lock(*context.directory_mutex);
      fs::path output_file_path(output_path_str);
      std::error_code ec;
      fs::create_directories(output_file_path.parent_path(), ec);
      if (ec) {
        std::lock_guard<std::mutex> console_lock(*context.console_mutex);
        LOG_ERROR("Failed to create output directory for %s: %s",
                  output_path_str.c_str(),
                  ec.message().c_str());
        return;
      }
    }

    auto create_file_stream = [&]() -> std::shared_ptr<std::ofstream> {
      auto stream = std::make_shared<std::ofstream>();
      stream->open(output_path_str,
                   std::ios::binary | std::ios::out | std::ios::trunc);
      if (!stream->is_open()) {
        throw std::runtime_error("Failed to open output file: " +
                                 output_path_str);
      }
      return stream;
    };

    std::shared_ptr<std::ofstream> output_stream;
    try {
      output_stream = RetryHelper::WaitForRetry(
          create_file_stream,
          5,
          1,
          5,
          [&](int current, int total, int timeout, int step) {
            std::lock_guard<std::mutex> lock(*context.console_mutex);
            LOG_INFO("Retrying file creation for %s (attempt %d/%d)",
                     output_path_str.c_str(),
                     current,
                     total);
          },
          context.cancel_flag.get());
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> lock(*context.console_mutex);
      LOG_ERROR("Failed to create output file after retries: %s - %s",
                output_path_str.c_str(),
                e.what());
      return;
    }

    struct FileStreamGuard {
      std::shared_ptr<std::ofstream> stream;
      ~FileStreamGuard() {
        if (stream && stream->is_open()) {
          stream->close();
        }
      }
    } file_guard{output_stream};

    auto download_asset = [&]() {
      // Wait on the async write so exceptions (timeouts, dropped
      // connections) propagate to WaitForRetry. Without .get() the returned
      // future is destroyed immediately and its destructor swallows the
      // exception, so retries never trigger and the output file stays empty.
      asset->write_to_stream_sequential(task_http_client, output_stream).get();
    };

    try {
      int retry_count = DownloadConfig::instance().GetRetryOnDownloadFailure();
      if (retry_count <= 0) retry_count = 30;

      RetryHelper::WaitForRetry(
          download_asset,
          retry_count,
          5,
          3,
          [&](int current, int total, int timeout, int step) {
            std::lock_guard<std::mutex> lock(*context.console_mutex);
            LOG_INFO("Retrying download for %s (attempt %d/%d)",
                     asset->name_.c_str(),
                     current,
                     total);
          },
          context.cancel_flag.get());
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> lock(*context.console_mutex);
      LOG_ERROR("Failed to download %s after retries: %s",
                asset->name_.c_str(),
                e.what());
      return;
    }

    output_stream->close();

    bool size_match = (fs::file_size(output_path_str) == asset->size_);
    std::string actual_md5 = ChecksumUtils::calculate_md5_file(output_path_str);
    bool md5_match = (actual_md5 == asset->hash_);

    if (!size_match || !md5_match) {
      std::lock_guard<std::mutex> lock(*context.console_mutex);
      LOG_ERROR("File verification failed for %s: size_match=%d, md5_match=%d",
                asset->name_.c_str(),
                size_match,
                md5_match);
      return;
    }

    std::string calculated_xxhash =
        ChecksumUtils::calculate_xxhash_file(output_path_str);
    if (calculated_xxhash.empty()) {
      std::lock_guard<std::mutex> lock(*context.console_mutex);
      LOG_ERROR("Failed to calculate XXHash for %s", asset->name_.c_str());
      return;
    }

    SaveFileRecordToDatabase(
        asset, context, manifest_pair, package, true, calculated_xxhash);

    {
      std::lock_guard<std::mutex> lock(*context.console_mutex);
      LOG_INFO("Downloaded and verified: %s (%llu bytes, MD5: %s, XXHash: %s)",
               asset->name_.c_str(),
               asset->size_,
               actual_md5.c_str(),
               calculated_xxhash.c_str());
    }

    if (context.progress_tracker) {
      context.progress_tracker->update_current_file(
          asset->name_, FileProcessStage::kDownloadCompleted);
      context.progress_tracker->update_current_file(
          asset->name_, FileProcessStage::kVerified);
      context.progress_tracker->increment_completed();
    }

    (*context.completed_downloads)++;

  } catch (const std::exception& e) {
    std::lock_guard<std::mutex> lock(*context.console_mutex);
    LOG_ERROR("Error downloading %s: %s", asset->name_.c_str(), e.what());
    (*context.completed_downloads)++;
  }
}

void ChunkDownloadService::WaitForDownloadCompletion(TaskQueue& download_queue,
                                                     DownloadContext& context) {
  auto start_wait = std::chrono::steady_clock::now();
  int timeout_minutes = chunk_service_config_.download_timeout_minutes;
  if (timeout_minutes <= 0) {
    timeout_minutes = DownloadConfig::instance().GetDownloadTimeoutMinutes();
  }
  const auto kTimeout = std::chrono::minutes(timeout_minutes);

  LOG_DEBUG(
      "Starting to wait for completion. Total downloads: %zu, timeout: %d "
      "minutes",
      context.total_downloads->load(),
      timeout_minutes);

  while (download_queue.ActiveTasks() > 0 || download_queue.QueueSize() > 0) {
    if (*context.cancel_flag) {
      LOG_DEBUG("Cancel flag set, exiting wait loop");
      break;
    }

    if (*context.completed_downloads >= *context.total_downloads) {
      std::lock_guard<std::mutex> lock(*context.console_mutex);
      LOG_DEBUG("All downloads completed! (%zu/%zu)",
                context.completed_downloads->load(),
                context.total_downloads->load());
      break;
    }

    auto now = std::chrono::steady_clock::now();
    if (now - start_wait > kTimeout) {
      std::lock_guard<std::mutex> lock(*context.console_mutex);
      LOG_WARN("Download timeout after %d minutes. Completed: %zu/%zu",
               timeout_minutes,
               context.completed_downloads->load(),
               context.total_downloads->load());
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  LOG_DEBUG(
      "Exited wait loop. Final stats - Active: %zu, Queue: %zu, Completed: "
      "%zu/%zu",
      download_queue.ActiveTasks(),
      download_queue.QueueSize(),
      context.completed_downloads->load(),
      context.total_downloads->load());
}

void ChunkDownloadService::SaveFileRecordToDatabase(
    const std::shared_ptr<Resource>& asset,
    const DownloadContext& context,
    const ChunkManifestPair& manifest_pair,
    const std::string& package,
    bool is_successful,
    const std::string& xxhash_checksum) {
  try {
    auto& db = DatabaseManager::Instance();

    std::string final_package_id = manifest_pair.get_api_package_id();
    if (final_package_id.empty()) {
      final_package_id = package.empty() ? "game" : package;
    }
    std::string final_build_id =
        !manifest_pair.get_api_build_id().empty()
            ? manifest_pair.get_api_build_id()
            : manifest_pair.get_manifest_metadata().get_id();
    std::string final_version = !manifest_pair.get_api_tag().empty()
                                    ? manifest_pair.get_api_tag()
                                    : final_build_id;
    std::string final_depot_id = !manifest_pair.get_api_category_id().empty()
                                     ? manifest_pair.get_api_category_id()
                                     : (final_package_id + "_depot");

    std::string checksum_to_use = xxhash_checksum;
    if (checksum_to_use.empty()) {
      std::string asset_name_fixed = asset->name_;
#ifdef _WIN32
      std::replace(asset_name_fixed.begin(), asset_name_fixed.end(), '/', '\\');
#endif
      fs::path output_path =
          fs::path(context.output_directory) / asset_name_fixed;
      std::string output_path_str = output_path.string();

      checksum_to_use = ChecksumUtils::calculate_xxhash_file(output_path_str);
      if (checksum_to_use.empty()) {
        LOG_ERROR("Failed to calculate XXHash for %s, skipping database record",
                  asset->name_.c_str());
        return;
      }
    }

    FileRecord file_record;
    file_record.package_id = final_package_id;
    file_record.branch = "main";
    file_record.build_id = final_build_id;
    file_record.depot_id = final_depot_id;
    file_record.file_id = asset->name_;
    file_record.file_ver = final_version;
    file_record.install_dir = context.output_directory;
    file_record.file_checksum = checksum_to_use;
    file_record.file_status = is_successful ? 1 : 0;

    if (db.InsertFileRecord(file_record)) {
      LOG_DEBUG("File record saved to database: %s", asset->name_.c_str());
    }
  } catch (const std::exception& ex) {
    LOG_ERROR("Failed to save file record to database: %s - %s",
              asset->name_.c_str(),
              ex.what());
  }
}

void ChunkDownloadService::SaveManifestAndConfigRecords(
    const ChunkManifestPair& manifest_pair,
    const std::string& package,
    const std::string& output_directory) {
  try {
    auto& db = DatabaseManager::Instance();
    std::string package_id = package.empty() ? "game" : package;
    std::string build_id = !manifest_pair.get_api_build_id().empty()
                               ? manifest_pair.get_api_build_id()
                               : manifest_pair.get_manifest_metadata().get_id();
    std::string version = !manifest_pair.get_api_tag().empty()
                              ? manifest_pair.get_api_tag()
                              : build_id;
    std::string depot_id = !manifest_pair.get_api_category_id().empty()
                               ? manifest_pair.get_api_category_id()
                               : (package_id + "_depot");
    std::string matching_field = !manifest_pair.get_api_matching_field().empty()
                                     ? manifest_pair.get_api_matching_field()
                                     : package;

    ManifestRecord manifest_record;
    manifest_record.package_id = package_id;
    manifest_record.build_id = build_id;
    manifest_record.depot_id = depot_id;
    manifest_record.version = version;
    manifest_record.depot_manifest_id =
        manifest_pair.get_manifest_metadata().get_id();
    manifest_record.depot_manifest_checksum =
        manifest_pair.get_manifest_metadata().get_checksum_md5();
    manifest_record.depot_manifest_compressed_size =
        manifest_pair.get_manifest_metadata().get_compressed_size();
    manifest_record.depot_manifest_uncompressed_size =
        manifest_pair.get_manifest_metadata().get_size();
    manifest_record.depot_manifest_url_prefix =
        manifest_pair.get_chunk_info().get_base_url();
    manifest_record.depot_manifest_encryption = 0;
    manifest_record.depot_manifest_password = "";
    manifest_record.depot_manifest_compression =
        manifest_pair.get_manifest_metadata().get_use_compression() ? 1 : 0;
    manifest_record.depot_manifest_md5 =
        manifest_pair.get_manifest_metadata().get_checksum_md5();
    db.InsertManifestRecord(manifest_record);
    db.DeleteOldManifestRecords(manifest_record.package_id, 2);

    ConfigRecord config_record;
    config_record.package_id = package_id;
    config_record.local_version = version;
    config_record.server_version = version;
    config_record.local_build_id = build_id;
    config_record.server_build_id = build_id;
    config_record.branch = "main";
    config_record.depot_id = depot_id;
    config_record.matching_field = matching_field;
    config_record.install_dir = output_directory;

    db.UpdateChunkConfig(config_record);

    LOG_INFO(
        "Manifest and config records saved to database: %s v%s (build: %s)",
        package_id.c_str(),
        version.c_str(),
        build_id.c_str());

    try {
      std::string manifest_dir = chunk_service_config_.manifest_output_dir;
      if (manifest_dir.empty()) {
        manifest_dir = PathHelper::GetManifestDir();
      }
      std::string mapping_file = manifest_dir + "/manifest_mapping.json";

      nlohmann::json mapping;

      if (fs::exists(mapping_file)) {
        std::ifstream ifs(mapping_file);
        if (ifs.is_open()) {
          try {
            ifs >> mapping;
            if (!mapping.contains("manifests") ||
                !mapping["manifests"].is_array()) {
              mapping = nlohmann::json::object();
              mapping["manifests"] = nlohmann::json::array();
            }
          } catch (...) {
            mapping = nlohmann::json::object();
            mapping["manifests"] = nlohmann::json::array();
          }
          ifs.close();
        }
      } else {
        mapping = nlohmann::json::object();
        mapping["manifests"] = nlohmann::json::array();
      }

      nlohmann::json manifest_entry = {
          {"manifest_id", manifest_pair.get_manifest_metadata().get_id()},
          {"package", package_id},
          {"usage", "chunk"},
          {"game_ver", version}};

      if (!manifest_pair.get_api_category_id().empty()) {
        manifest_entry["category_id"] = manifest_pair.get_api_category_id();
      }
      if (!manifest_pair.get_api_matching_field().empty()) {
        manifest_entry["matching_field"] =
            manifest_pair.get_api_matching_field();
      }

      auto& manifests = mapping["manifests"];
      bool found = false;
      for (auto& entry : manifests) {
        if (entry.contains("manifest_id") &&
            entry["manifest_id"] ==
                manifest_pair.get_manifest_metadata().get_id()) {
          entry = manifest_entry;
          found = true;
          LOG_INFO("Updated existing chunk manifest mapping for: %s",
                   manifest_pair.get_manifest_metadata().get_id().c_str());
          break;
        }
      }

      if (!found) {
        manifests.push_back(manifest_entry);
        LOG_INFO("Added new chunk manifest mapping for: %s",
                 manifest_pair.get_manifest_metadata().get_id().c_str());
      }

      std::ofstream ofs(mapping_file);
      if (ofs.is_open()) {
        ofs << mapping.dump(2);
        ofs.close();
        LOG_INFO("Saved chunk manifest mapping: %s -> %s (version: %s)",
                 manifest_pair.get_manifest_metadata().get_id().c_str(),
                 package_id.c_str(),
                 version.c_str());
      } else {
        LOG_WARN("Failed to save manifest mapping file");
      }
    } catch (const std::exception& e) {
      LOG_WARN("Failed to save manifest mapping: %s", e.what());
    }
  } catch (const std::exception& e) {
    LOG_ERROR("Failed to save manifest/config records: %s", e.what());
  }
}

QUATON_NAMESPACE_END
