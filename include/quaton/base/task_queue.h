#ifndef QUATON_BASE_TASK_QUEUE_H_
#define QUATON_BASE_TASK_QUEUE_H_
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "quaton/quaton_global.h"

QUATON_NAMESPACE_BEGIN

/**
 * @class TaskQueue
 * @brief Thread pool task queue class
 *
 * Provides thread pool functionality for concurrent execution of tasks.
 * Similar to .NET's ActionBlock pattern.
 */
class QUATON_API TaskQueue {
 public:
  /**
   * @brief Constructor
   * @param maxConcurrency Maximum number of concurrent worker threads
   */
  explicit TaskQueue(size_t maxConcurrency);

  /**
   * @brief Destructor - stops all threads and waits for completion
   */
  ~TaskQueue();

  // Non-copyable and non-movable
  QUATON_DISABLE_COPY_MOVE(TaskQueue);

  /**
   * @brief Add task to the queue for execution
   * @param task Function to execute
   */
  void Enqueue(std::function<void()> task);

  /**
   * @brief Stop task queue and wait for all threads to finish
   */
  void Stop();

  /**
   * @brief Get the number of tasks currently being executed
   * @return Active task count
   */
  size_t ActiveTasks() const;

  /**
   * @brief Get the number of tasks waiting in the queue
   * @return Queue size
   */
  size_t QueueSize() const;

  /**
   * @brief Check if queue is stopped
   * @return True if stopped
   */
  bool IsStopped() const { return stop_.load(); }

 private:
  std::queue<std::function<void()>> tasks_;  ///< Task queue
  std::vector<std::thread> workers_;         ///< Worker threads
  mutable std::mutex queueMutex_;  ///< Queue mutex (mutable for const methods)
  std::condition_variable
      condition_;           ///< Condition variable for task notification
  std::atomic<bool> stop_;  ///< Stop flag
  std::atomic<size_t> activeTasks_;  ///< Number of active tasks
  size_t maxConcurrency_;            ///< Maximum concurrency level
};

QUATON_NAMESPACE_END

#endif  // QUATON_BASE_TASK_QUEUE_H_
