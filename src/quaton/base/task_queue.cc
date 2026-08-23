#include "quaton/base/task_queue.h"

QUATON_NAMESPACE_BEGIN

TaskQueue::TaskQueue(size_t maxConcurrency)
    : stop_(false), activeTasks_(0), maxConcurrency_(maxConcurrency) {
  workers_.reserve(maxConcurrency);
  for (size_t i = 0; i < maxConcurrency; ++i) {
    workers_.emplace_back([this] {
      while (true) {
        std::function<void()> task;
        {
          std::unique_lock<std::mutex> lock(queueMutex_);
          condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });

          if (stop_ && tasks_.empty()) {
            return;
          }

          task = std::move(tasks_.front());
          tasks_.pop();
        }

        ++activeTasks_;
        // RAII guard for activeTasks
        struct ActiveTaskGuard {
          std::atomic<size_t>& counter;
          ~ActiveTaskGuard() { --counter; }
        } guard{activeTasks_};

        task();
      }
    });
  }
}

TaskQueue::~TaskQueue() {
  Stop();
}

void TaskQueue::Enqueue(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (stop_) {
      return;  // Don't accept new tasks after stop
    }
    tasks_.push(std::move(task));
  }
  condition_.notify_one();
}

void TaskQueue::Stop() {
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    stop_ = true;
  }
  condition_.notify_all();

  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

size_t TaskQueue::ActiveTasks() const {
  return activeTasks_.load();
}

size_t TaskQueue::QueueSize() const {
  std::lock_guard<std::mutex> lock(queueMutex_);
  return tasks_.size();
}

QUATON_NAMESPACE_END
