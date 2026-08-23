#include "quaton/base/download_item.h"

#include <chrono>

#include "quaton/logger.h"

namespace Quaton {

DownloadItem::DownloadItem(const std::string& id) : id_(id) {
  StateTransition initial;
  initial.from_state = DownloadState::kIdle;
  initial.to_state = DownloadState::kIdle;
  initial.reason = "Created";
  initial.timestamp_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  state_history_.push_back(initial);
}

bool DownloadItem::TransitionTo(DownloadState new_state,
                                const std::string& reason) {
  DownloadState old_state = state_.load();

  if (!IsValidTransition(old_state, new_state)) {
    LOG_WARN("Invalid state transition for item %s: %s -> %s",
             id_.c_str(),
             DownloadStateToString(old_state),
             DownloadStateToString(new_state));
    return false;
  }

  // Perform atomic transition
  if (!state_.compare_exchange_strong(old_state, new_state)) {
    // State changed by another thread, retry
    return TransitionTo(new_state, reason);
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    StateTransition transition;
    transition.from_state = old_state;
    transition.to_state = new_state;
    transition.reason = reason;
    transition.timestamp_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    state_history_.push_back(transition);
  }

  LOG_DEBUG("Item %s: %s -> %s (%s)",
            id_.c_str(),
            DownloadStateToString(old_state),
            DownloadStateToString(new_state),
            reason.c_str());

  // Notify callback
  if (state_callback_) {
    state_callback_(old_state, new_state);
  }

  // Call subclass hook
  OnStateChanged(old_state, new_state);

  return true;
}

void DownloadItem::SetStateChangeCallback(StateChangeCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_callback_ = std::move(callback);
}

bool DownloadItem::IsTerminal() const {
  auto state = state_.load();
  return state == DownloadState::kCompleted ||
         state == DownloadState::kFailed ||
         state == DownloadState::kCancelled || state == DownloadState::kSkipped;
}

bool DownloadItem::CanRetry() const {
  if (state_.load() != DownloadState::kFailed) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  return error_.is_retryable && error_.retry_count < max_retry_count_;
}

DownloadProgress DownloadItem::GetProgress() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return progress_;
}

void DownloadItem::UpdateProgress(int64_t downloaded_bytes,
                                  int64_t total_bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  progress_.downloaded_bytes = downloaded_bytes;
  progress_.total_bytes = total_bytes;
  progress_.UpdatePercentage();
}

void DownloadItem::UpdateSpeed(double bytes_per_sec) {
  std::lock_guard<std::mutex> lock(mutex_);
  progress_.speed_bytes_per_sec = bytes_per_sec;

  if (bytes_per_sec > 0 && progress_.total_bytes > progress_.downloaded_bytes) {
    int64_t remaining = progress_.total_bytes - progress_.downloaded_bytes;
    progress_.eta_seconds = static_cast<int64_t>(remaining / bytes_per_sec);
  } else {
    progress_.eta_seconds = 0;
  }
}

DownloadError DownloadItem::GetError() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return error_;
}

void DownloadItem::SetError(int code,
                            const std::string& message,
                            const std::string& detail,
                            bool retryable) {
  std::lock_guard<std::mutex> lock(mutex_);
  error_.error_code = code;
  error_.error_message = message;
  error_.error_detail = detail;
  error_.is_retryable = retryable;
}

void DownloadItem::ClearError() {
  std::lock_guard<std::mutex> lock(mutex_);
  error_.Clear();
}

int DownloadItem::IncrementRetryCount() {
  std::lock_guard<std::mutex> lock(mutex_);
  return ++error_.retry_count;
}

std::vector<StateTransition> DownloadItem::GetStateHistory() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_history_;
}

}  // namespace Quaton
