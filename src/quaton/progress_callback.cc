#include "quaton/progress_callback.h"

#include <algorithm>

namespace Quaton {

ProgressTracker::ProgressTracker(int total_files, OperationMode mode)
    : total_files_(total_files),
      completed_files_(0),
      bytes_downloaded_(0),
      bytes_total_(0),
      operation_mode_(mode),
      last_bytes_downloaded_(0),
      current_speed_(0.0) {
  start_time_ = std::chrono::steady_clock::now();
  last_callback_time_ = start_time_;
  last_speed_update_ = start_time_;
}

void ProgressTracker::set_callback(ProgressCallback callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  callback_ = std::move(callback);
}

void ProgressTracker::update_current_file(const std::string& file_path,
                                          FileProcessStage stage) {
  {
    std::lock_guard<std::mutex> lock(current_file_mutex_);
    current_file_ = file_path;
    current_status_.set_stage(stage, operation_mode_);
  }

  trigger_callback_if_ready();
}

void ProgressTracker::increment_completed() {
  completed_files_++;

  {
    std::lock_guard<std::mutex> lock(current_file_mutex_);
    current_status_.reset();
  }

  trigger_callback_if_ready();
}

void ProgressTracker::update_download_stats(int64_t bytes_downloaded,
                                            int64_t bytes_total) {
  bytes_downloaded_ = bytes_downloaded;
  bytes_total_ = bytes_total;

  auto now = std::chrono::steady_clock::now();
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - last_speed_update_)
                        .count();

  if (elapsed_ms >= 1000) {  // Update speed every second
    int64_t bytes_delta = bytes_downloaded - last_bytes_downloaded_;
    if (elapsed_ms > 0) {
      current_speed_ = (bytes_delta * 1000.0) / elapsed_ms;  // bytes/second
    }
    last_bytes_downloaded_ = bytes_downloaded;
    last_speed_update_ = now;
  }

  trigger_callback_if_ready();
}

void ProgressTracker::force_update() {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  if (callback_) {
    callback_(get_progress());
  }
  last_callback_time_ = std::chrono::steady_clock::now();
}

ProgressInfo ProgressTracker::get_progress() const {
  ProgressInfo info;

  // Basic counts
  info.total_files = total_files_.load();
  info.completed_files = completed_files_.load();
  info.remaining_files = info.total_files - info.completed_files;

  // Calculate overall percentage
  if (info.total_files > 0) {
    double file_progress =
        static_cast<double>(info.completed_files) / info.total_files;

    // If we have byte information, use it for more accurate progress
    int64_t bytes_total = bytes_total_.load();
    if (bytes_total > 0) {
      double byte_progress =
          static_cast<double>(bytes_downloaded_.load()) / bytes_total;
      // Weight: 50% file count, 50% byte progress
      info.overall_percentage = (file_progress + byte_progress) / 2.0;
    } else {
      info.overall_percentage = file_progress;
    }
  } else {
    info.overall_percentage = 0.0;
  }

  // Clamp to [0, 1]
  info.overall_percentage =
      std::max(0.0, std::min(1.0, info.overall_percentage));

  // Current file info
  {
    std::lock_guard<std::mutex> lock(current_file_mutex_);
    info.current_file = current_file_;
    info.current_status = current_status_;
  }

  // Download speed (convert to MB/s)
  info.download_speed = current_speed_ / (1024.0 * 1024.0);

  // Estimated time
  if (current_speed_ > 0 && bytes_total_.load() > 0) {
    int64_t remaining_bytes = bytes_total_.load() - bytes_downloaded_.load();
    if (remaining_bytes > 0) {
      info.estimated_seconds = remaining_bytes / current_speed_;
      info.estimated_minutes = static_cast<int>(info.estimated_seconds / 60);
      info.estimated_remaining_seconds =
          static_cast<int>(info.estimated_seconds) % 60;
    }
  }

  // Operation mode
  info.operation_mode = operation_mode_;

  return info;
}

void ProgressTracker::reset() {
  completed_files_ = 0;
  bytes_downloaded_ = 0;
  bytes_total_ = 0;
  last_bytes_downloaded_ = 0;
  current_speed_ = 0.0;

  {
    std::lock_guard<std::mutex> lock(current_file_mutex_);
    current_file_.clear();
    current_status_.reset();
  }

  start_time_ = std::chrono::steady_clock::now();
  last_callback_time_ = start_time_;
  last_speed_update_ = start_time_;
}

bool ProgressTracker::should_trigger_callback() {
  auto now = std::chrono::steady_clock::now();
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - last_callback_time_)
                        .count();
  return elapsed_ms >= kCallbackIntervalMs;
}

void ProgressTracker::trigger_callback_if_ready() {
  if (!should_trigger_callback()) {
    return;
  }

  std::lock_guard<std::mutex> lock(callback_mutex_);
  if (callback_) {
    callback_(get_progress());
    last_callback_time_ = std::chrono::steady_clock::now();
  }
}

}  // namespace Quaton
