#include "quaton/downloader/speed_limiter.h"

QUATON_NAMESPACE_BEGIN

std::shared_ptr<SpeedLimiter::State> SpeedLimiter::CreateState(
    long initial_speed) {
  auto state = std::make_shared<State>();
  state->initial_requested_speed = initial_speed;
  return state;
}

std::function<void(void*, long)> SpeedLimiter::GetSpeedListener(
    std::shared_ptr<State> state) {
  return [state](void* sender, long new_requested_speed) {
    SpeedChangeListener(state, sender, new_requested_speed);
  };
}

void SpeedLimiter::SetChunkProcessingChangedHandler(
    std::shared_ptr<State> state, ChunkProcessingChangedHandler handler) {
  state->chunk_processing_changed_handler = handler;
}

void SpeedLimiter::SetDownloadSpeedChangedHandler(
    std::shared_ptr<State> state, DownloadSpeedChangedHandler handler) {
  state->download_speed_changed_handler = handler;
}

void SpeedLimiter::IncrementChunkCount(std::shared_ptr<State> state) {
  int new_count = ++state->current_chunk_processing;
  if (state->chunk_processing_changed_handler) {
    state->chunk_processing_changed_handler(new_count);
  }
}

void SpeedLimiter::DecrementChunkCount(std::shared_ptr<State> state) {
  int new_count = --state->current_chunk_processing;
  if (state->chunk_processing_changed_handler) {
    state->chunk_processing_changed_handler(new_count);
  }
}

int SpeedLimiter::GetCurrentChunkCount(const std::shared_ptr<State>& state) {
  return state->current_chunk_processing.load();
}

long SpeedLimiter::GetInitialSpeed(const std::shared_ptr<State>& state) {
  return state->initial_requested_speed;
}

void SpeedLimiter::SetInitialSpeed(std::shared_ptr<State> state, long speed) {
  state->initial_requested_speed = speed;
}

void SpeedLimiter::SpeedChangeListener(std::shared_ptr<State> state,
                                       void* sender,
                                       long new_requested_speed) {
  if (state->download_speed_changed_handler) {
    state->download_speed_changed_handler(new_requested_speed);
  }
  state->initial_requested_speed = new_requested_speed;
}

QUATON_NAMESPACE_END
