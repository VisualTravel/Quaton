#include "quaton/logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>

#include "quaton/utils/path_helper.h"

#ifdef OS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX  // Prevent Windows.h from defining min/max macros
#endif
#include <direct.h>
#include <windows.h>
#define mkdir(dir, mode) _mkdir(dir)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

class Logger::LoggerImpl {
 public:
  LoggerImpl()
      : initialized(false),
        currentFileSize(0),
        currentLogIndex(0),
        minLogLevel(LogLevel::Trace) {}

  ~LoggerImpl() { exitFileLogger(); }

  bool initialize() {
    std::lock_guard<std::mutex> lock(mutex);
    // Idempotent: a second call must not start a second worker thread.
    if (initialized.load()) {
      return true;
    }
    if (!initFileLogger()) {
      return false;
    }
    initialized.store(true);

    // The worker is started here, under the same lock that stops it, so a
    // shutdown can never race with a lazy start and leave an unjoined thread
    // (or an unreachable worker) behind.
    logWorkerThread = std::thread([this]() { this->processLogQueue(); });

    logFile << "\n\n\n\n\n";
    logFile << "//////////////////// Quaton Started ////////////////////"
            << std::endl;
    logFile << "//////////////////// Hello World! ////////////////////"
            << std::endl;
    logFile.flush();

    return true;
  }

  void setLogLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex);
    minLogLevel = level;
  }

  bool initFileLogger() {
    logDirectory = Quaton::PathHelper::GetLogsDir();

    std::error_code error;
    if (!std::filesystem::exists(logDirectory, error)) {
      std::filesystem::create_directories(logDirectory, error);
    }

    logFilename = logDirectory + "/" + lastLogFilename;
    logFile.open(logFilename, std::ios::app);
    if (!logFile.is_open()) {
      return false;
    }

    currentFileSize = std::filesystem::file_size(logFilename, error);
    if (error) {
      currentFileSize = 0;
    }
    return true;
  }

  void log(LogLevel level,
           const char* file,
           int line,
           const char* fmt,
           va_list args) {
    if (!initialized) {
      return;
    }

    if (level < minLogLevel.load()) {
      return;
    }

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, args);

    std::string formattedMessage = formatLogMessage(level, file, line, buffer);
    asyncLog(level, formattedMessage);
  }

  void asyncLog(LogLevel level, std::string message) {
    std::lock_guard<std::mutex> lock(mutex);
    const bool was_empty = logQueue.empty();
    logQueue.push(std::move(message));
    // Wake the worker for the first message of a batch as well, otherwise a
    // short burst could sit unflushed until the queue fills up.
    if (was_empty || logQueue.size() >= 16) {
      logCondition.notify_one();
    }
  }

  void processLogQueue() {
    while (true) {
      std::unique_lock<std::mutex> lock(mutex);
      logCondition.wait(lock,
                        [this]() { return !logQueue.empty() || !initialized; });
      if (!initialized) {
        break;
      }

      // Everything below runs under the lock - logFile and currentFileSize are
      // only ever touched under it, which is also why the rotation goes through
      // saveLogLocked() instead of the public saveLog() - and inside a
      // catch-all, because an exception escaping this thread would terminate
      // the host process.
      try {
        std::vector<std::string> batch;
        while (!logQueue.empty() && batch.size() < 32) {
          batch.push_back(std::move(logQueue.front()));
          logQueue.pop();
        }

        // A failed rotation can leave the stream closed; retry the reopen here
        // so logging recovers instead of staying silent forever.
        if (!logFile.is_open()) {
          logFile.clear();
          logFile.open(logFilename, std::ios::app);
        }

        if (logFile.is_open()) {
          for (const auto& logMessage : batch) {
            if (currentFileSize >= maxFileSize) {
              saveLogLocked();
            }
            logFile << logMessage << std::endl;
            currentFileSize += logMessage.size() + 1;
          }
          logFile.flush();
        }
      } catch (...) {
        // Drop the batch and keep serving later messages.
      }
    }
  }

  std::string formatLogMessage(LogLevel level,
                               const char* file,
                               int line,
                               const std::string& message) {
    // Plain string handling: this runs on the caller's thread, which may be
    // outside any try/catch, so no throwing filesystem call belongs here.
    const std::string file_path = (file == nullptr) ? "" : file;
    const size_t separator = file_path.find_last_of("/\\");
    const std::string file_name = (separator == std::string::npos)
                                      ? file_path
                                      : file_path.substr(separator + 1);

    std::stringstream ss;
    ss << "[" << getCurrentTimestamp() << "]"
       << "[" << getThreadId() << "]"
       << "[" << getLogLevelString(level) << "]"
       << "[" << file_name << ":" << line << "] " << message;
    return ss.str();
  }

  std::string getLogLevelString(LogLevel level) {
    switch (level) {
      case LogLevel::Trace:
        return "Trace";
      case LogLevel::Debug:
        return "Debug";
      case LogLevel::Info:
        return "Info";
      case LogLevel::Warn:
        return "Warn";
      case LogLevel::Error:
        return "Error";
      case LogLevel::Fatal:
        return "Fatal";
      default:
        return "Unknown";
    }
  }

  std::string getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) %
              1000;

    std::stringstream ss;
    // Use thread-safe localtime_s (Windows) or localtime_r (POSIX)
    struct tm timeinfo = {};
#ifdef OS_WINDOWS
    localtime_s(&timeinfo, &in_time_t);
#else
    localtime_r(&in_time_t, &timeinfo);
#endif

    ss << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S") << "."
       << std::setfill('0') << std::setw(3) << ms.count();

    return ss.str();
  }

  std::string getThreadId() {
    std::stringstream ss;
#ifdef OS_WINDOWS
    ss << std::this_thread::get_id();
#else
    ss << pthread_self();
#endif
    return ss.str();
  }

  void saveLog() {
    std::lock_guard<std::mutex> lock(mutex);
    saveLogLocked();
  }

  // Rotates the log file. The caller must hold mutex.
  void saveLogLocked() {
    if (!logFile.is_open()) {
      return;
    }

    logFile.close();
    const std::string newFilename =
        logDirectory + "/" + generateLogFilenameUnsafe();

    // Rotation is best effort: the rename fails while another process still
    // holds the log file, and throwing here would kill the log worker thread
    // (and with it the whole process).
    std::error_code error;
    std::filesystem::rename(logFilename, newFilename, error);

    logFile.clear();
    logFile.open(logFilename, std::ios::app);
    if (!logFile.is_open()) {
      // The worker reopens the stream before the next batch, so only reset the
      // counter here instead of leaving the rotation armed.
      currentFileSize = 0;
      return;
    }

    if (error) {
      // Keep appending to the current file and only retry the rotation after
      // another full file worth of output, so a locked log file cannot turn
      // into a rename loop.
      currentFileSize = 0;
      return;
    }

    std::error_code size_error;
    currentFileSize = std::filesystem::file_size(logFilename, size_error);
    if (size_error) {
      currentFileSize = 0;
    }
  }

  std::string generateLogFilenameUnsafe() {
    // Length of the "quaton." prefix that the rotated files share.
    static constexpr size_t kPrefixLength = 7;
    if (currentLogIndex == 0) {
      int maxIndex = 0;
      std::error_code error;
      // Enumeration is best effort; an unreadable directory must not abort the
      // rotation, so failures simply fall back to index 1.
      try {
        for (std::filesystem::directory_iterator it(logDirectory, error), last;
             !error && it != last;
             it.increment(error)) {
          const std::string filename = it->path().filename().string();
          if (filename.rfind("quaton.", 0) == 0 && filename != "quaton.log") {
            try {
              const size_t dotPos = filename.find('.', kPrefixLength);
              if (dotPos != std::string::npos && dotPos > kPrefixLength) {
                const int num = std::stoi(
                    filename.substr(kPrefixLength, dotPos - kPrefixLength));
                maxIndex = std::max(maxIndex, num);
              }
            } catch (...) {
              continue;
            }
          }
        }
      } catch (...) {
        maxIndex = 0;
      }
      currentLogIndex = maxIndex + 1;
    }
    return "quaton." + std::to_string(currentLogIndex++) + ".log";
  }

  void exitFileLogger() {
    // The stop flag must be changed while holding the lock the worker waits on,
    // otherwise the notification can be lost and the join below would hang the
    // process shutdown forever.
    std::thread worker;
    bool was_initialized = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      was_initialized = initialized.load();
      initialized.store(false);
      // Take the handle under the lock so the join cannot race with the start
      // in initialize().
      worker = std::move(logWorkerThread);
    }
    if (!was_initialized) {
      return;
    }

    logCondition.notify_all();

    if (worker.joinable()) {
      worker.join();
    }

    std::lock_guard<std::mutex> lock(mutex);
    if (logFile.is_open()) {
      logFile.flush();
      logFile.close();
    }
  }

  void exitLogs() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (logFile.is_open()) {
        logFile << "//////////////////// Log End ////////////////////"
                << std::endl;
        logFile << "\n\n\n\n\n";
        logFile.flush();
      }
      saveLogLocked();
    }
    exitFileLogger();
  }

  std::ofstream logFile;
  std::string logFilename;
  std::string logDirectory = "logs";
  std::string lastLogFilename = "quaton.log";
  static constexpr uint64_t maxFileSize = 10240 * 1024;
  uint64_t currentFileSize;
  std::mutex mutex;
  std::condition_variable logCondition;
  std::thread logWorkerThread;
  std::atomic<bool> initialized;
  std::queue<std::string> logQueue;
  std::atomic<int> currentLogIndex;
  // Read on every log call from any thread, so it is atomic instead of being
  // read outside the mutex.
  std::atomic<LogLevel> minLogLevel;
};

Logger::LoggerImpl* Logger::pImpl = nullptr;

bool Logger::initialize() {
  if (!pImpl) {
    pImpl = new LoggerImpl();
  }
  return pImpl->initialize();
}

void Logger::log(
    LogLevel level, const char* file, int line, const char* fmt, ...) {
  if (!pImpl) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  pImpl->log(level, file, line, fmt, args);
  va_end(args);
}

void Logger::saveLog() {
  if (pImpl) {
    pImpl->saveLog();
  }
}

void Logger::exitLogs() {
  if (pImpl) {
    pImpl->exitLogs();
    delete pImpl;
    pImpl = nullptr;
  }
}

void Logger::setLogLevel(LogLevel level) {
  if (pImpl) {
    pImpl->setLogLevel(level);
  }
}
