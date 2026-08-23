#ifndef QUATON_UTILS_RETRY_HELPER_H_
#define QUATON_UTILS_RETRY_HELPER_H_
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <thread>

namespace Quaton {

/**
 * @class RetryHelper
 * @brief Helper class for executing functions with retry mechanism
 *
 * Provides functionality to execute functions with configurable
 * timeout and retry behavior.
 */
class RetryHelper {
 public:
  /// Default timeout in seconds
  static constexpr int DefaultTimeoutSec = 20;

  /// Default retry attempts
  static constexpr int DefaultRetryAttempt = 10;

  /**
   * @brief Execute function with retry mechanism
   * @tparam Func Function type (must be callable and return a value)
   * @param func Function to execute
   * @param timeout Initial timeout in seconds between retries
   * @param timeoutStep Timeout increase per retry attempt
   * @param retryAttempt Maximum number of retry attempts
   * @param onRetry Callback function called on each retry (currentRetry,
   * totalRetries, timeout, timeoutStep)
   * @param cancelFlag Optional atomic flag to cancel operation
   * @return Function execution result
   * @throws std::runtime_error If operation is cancelled or all retries
   * exhausted
   */
  template <typename Func>
  static auto WaitForRetry(
      Func&& func,
      int timeout = DefaultTimeoutSec,
      int timeoutStep = 0,
      int retryAttempt = DefaultRetryAttempt,
      std::function<void(int, int, int, int)> onRetry = nullptr,
      std::atomic<bool>* cancelFlag = nullptr) {
    int currentRetry = 1;
    std::exception_ptr lastException;

    while (currentRetry <= retryAttempt) {
      // Check cancellation
      if (cancelFlag && *cancelFlag) {
        throw std::runtime_error("Operation cancelled");
      }

      try {
        return func();
      } catch (const std::exception& e) {
        lastException = std::current_exception();

        // Call retry callback if provided
        if (onRetry) {
          onRetry(currentRetry, retryAttempt, timeout, timeoutStep);
        }

        // If not last attempt, wait and continue
        if (currentRetry < retryAttempt) {
          std::this_thread::sleep_for(std::chrono::seconds(timeout));
          timeout += timeoutStep;
          currentRetry++;
        } else {
          break;
        }
      }
    }

    // Rethrow the last exception if any
    if (lastException) {
      std::rethrow_exception(lastException);
    }

    throw std::runtime_error("Retry failed");
  }

  /**
   * @brief Execute function with simple retry (no callback)
   * @tparam Func Function type
   * @param func Function to execute
   * @param retryAttempt Maximum number of retry attempts
   * @param delayMs Delay between retries in milliseconds
   * @return Function execution result
   */
  template <typename Func>
  static auto SimpleRetry(Func&& func,
                          int retryAttempt = 3,
                          int delayMs = 1000) {
    return WaitForRetry(std::forward<Func>(func),
                        delayMs / 1000,  // Convert to seconds
                        0,
                        retryAttempt,
                        nullptr,
                        nullptr);
  }
};

}  // namespace Quaton

#endif  // QUATON_UTILS_RETRY_HELPER_H_
