#ifndef QUATON_BASE_DOWNLOAD_STATE_H_
#define QUATON_BASE_DOWNLOAD_STATE_H_

#include <functional>
#include <string>

#include "quaton/quaton_global.h"

QUATON_NAMESPACE_BEGIN

/**
 * @enum DownloadState
 * @brief Download item state enumeration
 *
 * State transition diagram:
 *
 *  ┌─────────────┐
 *  │   kIdle     │ ──────────────────────────────────┐
 *  └─────┬───────┘                                   │
 *        │ Start()                                   │
 *        ▼                                           │
 *  ┌─────────────┐                                   │
 *  │  kPending   │ ◄─────────────────────────────────┼── Retry
 *  └─────┬───────┘                                   │
 *        │ Scheduler assigns                         │
 *        ▼                                           │
 *  ┌─────────────┐                                   │
 *  │ kDownloading│                                   │
 *  └─────┬───────┘                                   │
 *        │                                           │
 *   ┌────┴────┐                                      │
 *   │         │                                      │
 *   ▼         ▼                                      │
 * Success   Failure ─────────────────────────────────┘
 *   │
 *   ▼
 *  ┌─────────────┐
 *  │ kVerifying  │
 *  └─────┬───────┘
 *        │
 *   ┌────┴────┐
 *   │         │
 *   ▼         ▼
 * Success   Failure ─────────────────────────────────┐
 *   │                                                │
 *   ▼                                                │
 *  ┌─────────────┐     ┌─────────────┐              │
 *  │ kApplying   │ ──► │ kCompleted  │              │
 *  └─────────────┘     └─────────────┘              │
 *                                                    │
 *        ┌───────────────────────────────────────────┘
 *        ▼
 *  ┌─────────────┐     ┌─────────────┐
 *  │  kFailed    │     │ kCancelled  │
 *  └─────────────┘     └─────────────┘
 */
enum class DownloadState {
  kIdle = 0,     ///< Initial state, not started
  kPending,      ///< Queued, waiting for scheduler
  kDownloading,  ///< Currently downloading
  kPaused,       ///< Download paused
  kVerifying,    ///< Verifying downloaded content
  kApplying,     ///< Applying patches (for LDIFF)
  kCompleted,    ///< Successfully completed
  kFailed,       ///< Failed with error
  kCancelled,    ///< Cancelled by user
  kSkipped,      ///< Skipped (already exists/cached)
};

/**
 * @brief Convert DownloadState to string
 */
inline const char* DownloadStateToString(DownloadState state) {
  switch (state) {
    case DownloadState::kIdle:
      return "Idle";
    case DownloadState::kPending:
      return "Pending";
    case DownloadState::kDownloading:
      return "Downloading";
    case DownloadState::kPaused:
      return "Paused";
    case DownloadState::kVerifying:
      return "Verifying";
    case DownloadState::kApplying:
      return "Applying";
    case DownloadState::kCompleted:
      return "Completed";
    case DownloadState::kFailed:
      return "Failed";
    case DownloadState::kCancelled:
      return "Cancelled";
    case DownloadState::kSkipped:
      return "Skipped";
    default:
      return "Unknown";
  }
}

/**
 * @enum JobState
 * @brief Job execution state
 */
enum class JobState {
  kCreated = 0,  ///< Job created but not started
  kRunning,      ///< Job is running
  kSuspended,    ///< Job is suspended
  kCompleted,    ///< Job completed successfully
  kFailed,       ///< Job failed
  kCancelled,    ///< Job cancelled
};

/**
 * @brief Convert JobState to string
 */
inline const char* JobStateToString(JobState state) {
  switch (state) {
    case JobState::kCreated:
      return "Created";
    case JobState::kRunning:
      return "Running";
    case JobState::kSuspended:
      return "Suspended";
    case JobState::kCompleted:
      return "Completed";
    case JobState::kFailed:
      return "Failed";
    case JobState::kCancelled:
      return "Cancelled";
    default:
      return "Unknown";
  }
}

/**
 * @enum TaskPriority
 * @brief Task scheduling priority
 */
enum class TaskPriority {
  kLow = 0,
  kNormal = 1,
  kHigh = 2,
  kCritical = 3,
};

/**
 * @struct StateTransition
 * @brief Represents a state transition
 */
struct StateTransition {
  DownloadState from_state;
  DownloadState to_state;
  std::string reason;
  int64_t timestamp_ms;
};

/**
 * @brief State change callback type
 */
using StateChangeCallback =
    std::function<void(DownloadState old_state, DownloadState new_state)>;

/**
 * @brief Check if a state transition is valid
 */
inline bool IsValidTransition(DownloadState from, DownloadState to) {
  switch (from) {
    case DownloadState::kIdle:
      return to == DownloadState::kPending || to == DownloadState::kCancelled;

    case DownloadState::kPending:
      return to == DownloadState::kDownloading ||
             to == DownloadState::kSkipped || to == DownloadState::kCancelled;

    case DownloadState::kDownloading:
      return to == DownloadState::kVerifying || to == DownloadState::kPaused ||
             to == DownloadState::kFailed || to == DownloadState::kCancelled;

    case DownloadState::kPaused:
      return to == DownloadState::kDownloading ||
             to == DownloadState::kCancelled;

    case DownloadState::kVerifying:
      return to == DownloadState::kApplying ||
             to == DownloadState::kCompleted || to == DownloadState::kFailed ||
             to == DownloadState::kPending;  // Retry on verification failure

    case DownloadState::kApplying:
      return to == DownloadState::kCompleted || to == DownloadState::kFailed;

    case DownloadState::kFailed:
      return to == DownloadState::kPending;  // Allow retry

    case DownloadState::kCompleted:
    case DownloadState::kCancelled:
    case DownloadState::kSkipped:
      return false;  // Terminal states

    default:
      return false;
  }
}

QUATON_NAMESPACE_END

#endif  // QUATON_BASE_DOWNLOAD_STATE_H_
