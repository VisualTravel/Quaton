#ifndef QUATON_DOWNLOADER_SPEED_LIMITER_H_
#define QUATON_DOWNLOADER_SPEED_LIMITER_H_
#pragma once

#include <atomic>
#include <functional>
#include <memory>

#include "quaton/quaton_global.h"

QUATON_NAMESPACE_BEGIN

/**
 * @class SpeedLimiter
 * @brief Download speed limiter for controlling concurrent downloads
 *
 * This class manages download speed limiting by tracking the number
 * of concurrent chunk downloads and providing callbacks for state changes.
 */
class QUATON_API SpeedLimiter {
 public:
  /// @brief Event type for current chunk processing count changes
  using ChunkProcessingChangedHandler = std::function<void(int)>;

  /// @brief Download speed change event type
  using DownloadSpeedChangedHandler = std::function<void(long)>;

  /**
   * @struct State
   * @brief Download speed limiter state structure
   */
  struct State {
    std::atomic<int> current_chunk_processing{
        0};  ///< Current number of chunks being processed
    long initial_requested_speed{0};  ///< Initial requested speed
    ChunkProcessingChangedHandler
        chunk_processing_changed_handler;  ///< Chunk processing change handler
    DownloadSpeedChangedHandler
        download_speed_changed_handler;  ///< Download speed change handler
  };

  /**
   * @brief Create speed limiter state instance
   * @param initial_speed Initial requested speed
   * @return Speed limiter state instance
   */
  static std::shared_ptr<State> CreateState(long initial_speed);

  /**
   * @brief Get download speed listener
   * @param state Speed limiter state
   * @return Download speed change listener
   */
  static std::function<void(void*, long)> GetSpeedListener(
      std::shared_ptr<State> state);

  /**
   * @brief Set current chunk processing count change event handler
   * @param state Speed limiter state
   * @param handler Event handler
   */
  static void SetChunkProcessingChangedHandler(
      std::shared_ptr<State> state, ChunkProcessingChangedHandler handler);

  /**
   * @brief Set download speed change event handler
   * @param state Speed limiter state
   * @param handler Event handler
   */
  static void SetDownloadSpeedChangedHandler(
      std::shared_ptr<State> state, DownloadSpeedChangedHandler handler);

  /**
   * @brief Increment processed chunk count
   * @param state Speed limiter state
   */
  static void IncrementChunkCount(std::shared_ptr<State> state);

  /**
   * @brief Decrement processed chunk count
   * @param state Speed limiter state
   */
  static void DecrementChunkCount(std::shared_ptr<State> state);

  /**
   * @brief Get current number of chunks being processed
   * @param state Speed limiter state
   * @return Current chunk count
   */
  static int GetCurrentChunkCount(const std::shared_ptr<State>& state);

  /**
   * @brief Get initial requested speed
   * @param state Speed limiter state
   * @return Initial requested speed
   */
  static long GetInitialSpeed(const std::shared_ptr<State>& state);

  /**
   * @brief Set initial requested speed
   * @param state Speed limiter state
   * @param speed New requested speed
   */
  static void SetInitialSpeed(std::shared_ptr<State> state, long speed);

 private:
  SpeedLimiter() = default;  // Static class, no instantiation

  /**
   * @brief Download speed change listener
   * @param state Speed limiter state
   * @param sender Sender
   * @param new_requested_speed New requested speed
   */
  static void SpeedChangeListener(std::shared_ptr<State> state,
                                  void* sender,
                                  long new_requested_speed);
};

// Type alias for backward compatibility (will be removed later)
using DownloadSpeedLimiterState = SpeedLimiter::State;

QUATON_NAMESPACE_END

#endif  // QUATON_DOWNLOADER_SPEED_LIMITER_H_
