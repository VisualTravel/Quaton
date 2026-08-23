#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "quaton/quaton_global.h"

QUATON_NAMESPACE_BEGIN

/**
 * @class ThreadPool
 * @brief A thread pool for managing and executing concurrent tasks
 */
class QUATON_API ThreadPool {
 public:
  /**
   * @brief Constructor
   * @param thread_count Number of worker threads to create
   */
  explicit ThreadPool(size_t thread_count);

  /**
   * @brief Destructor - stops all threads and waits for completion
   */
  ~ThreadPool();

  // Disable copy constructor and copy assignment
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  /**
   * @brief Submit a task to the thread pool
   * @tparam F Function type
   * @tparam Args Argument types
   * @param f Function to execute
   * @param args Arguments to pass to the function
   * @return Future object for retrieving the result
   * @throws std::runtime_error if thread pool is stopped
   */
  template <class F, class... Args>
  auto enqueue(F&& f, Args&&... args)
      -> std::future<typename std::invoke_result_t<F, Args...>> {
    using return_type = typename std::invoke_result_t<F, Args...>;
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));
    std::future<return_type> res = task->get_future();
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      if (stop_) throw std::runtime_error("ThreadPool stopped");
      tasks_.emplace([task]() { (*task)(); });
    }
    cond_var_.notify_one();
    return res;
  }

 private:
  std::vector<std::thread> workers_;         ///< Worker threads
  std::queue<std::function<void()>> tasks_;  ///< Task queue
  std::mutex queue_mutex_;                   ///< Mutex for task queue
  std::condition_variable
      cond_var_;            ///< Condition variable for task notification
  std::atomic<bool> stop_;  ///< Flag to stop the thread pool

  /**
   * @brief Worker thread function
   */
  void worker();
};

QUATON_NAMESPACE_END
