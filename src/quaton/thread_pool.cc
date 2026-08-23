#include "quaton/thread_pool.h"

namespace Quaton {

ThreadPool::ThreadPool(size_t thread_count) : stop_(false) {
  for (size_t i = 0; i < thread_count; ++i) {
    workers_.emplace_back([this] { this->worker(); });
  }
}

ThreadPool::~ThreadPool() {
  stop_ = true;
  cond_var_.notify_all();
  for (auto& t : workers_) {
    if (t.joinable()) {
      t.join();
    }
  }
}

void ThreadPool::worker() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      cond_var_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
      if (stop_ && tasks_.empty()) {
        return;
      }
      task = std::move(tasks_.front());
      tasks_.pop();
    }
    task();
  }
}

}  // namespace Quaton
