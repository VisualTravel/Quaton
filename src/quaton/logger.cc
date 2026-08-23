#include "quaton/logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
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
    if (!initFileLogger()) {
      return false;
    }
    initialized = true;
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

    if (!std::filesystem::exists(logDirectory)) {
      std::filesystem::create_directories(logDirectory);
    }

    logFilename = logDirectory + "/" + lastLogFilename;
    logFile.open(logFilename, std::ios::app);
    if (!logFile.is_open()) {
      return false;
    }

    if (std::filesystem::exists(logFilename)) {
      currentFileSize = std::filesystem::file_size(logFilename);
    } else {
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

    if (level < minLogLevel) {
      return;
    }

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, args);

    std::string formattedMessage = formatLogMessage(level, file, line, buffer);
    asyncLog(level, formattedMessage);
  }

  void asyncLog(LogLevel level, const std::string& message) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      logQueue.push(message);
      if (logQueue.size() >= 16) {
        logCondition.notify_one();
      }
    }
    std::call_once(workerThreadFlag, [this]() {
      logWorkerThread = std::thread([this]() { this->processLogQueue(); });
    });
  }

  void processLogQueue() {
    while (initialized) {
      std::unique_lock<std::mutex> lock(mutex);
      logCondition.wait(lock,
                        [this]() { return !logQueue.empty() || !initialized; });

      std::vector<std::string> batch;
      while (!logQueue.empty() && batch.size() < 32) {
        batch.push_back(std::move(logQueue.front()));
        logQueue.pop();
      }
      lock.unlock();

      if (logFile.is_open()) {
        for (const auto& logMessage : batch) {
          if (currentFileSize >= maxFileSize) {
            saveLog();
          }
          logFile << logMessage << std::endl;
          currentFileSize += logMessage.size() + 1;
        }
        logFile.flush();
      }
      lock.lock();
    }
  }

  std::string formatLogMessage(LogLevel level,
                               const char* file,
                               int line,
                               const std::string& message) {
    std::stringstream ss;
    ss << "[" << getCurrentTimestamp() << "]"
       << "[" << getThreadId() << "]"
       << "[" << getLogLevelString(level) << "]"
       << "[" << std::filesystem::path(file).filename().string() << ":" << line
       << "] " << message;
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
    std::string newFilename;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (logFile.is_open()) {
        logFile.close();
        newFilename = logDirectory + "/" + generateLogFilenameUnsafe();
        std::filesystem::rename(logFilename, newFilename);
        logFile.open(logFilename, std::ios::app);
        if (logFile.is_open()) {
          currentFileSize = std::filesystem::file_size(logFilename);
        } else {
          currentFileSize = 0;
        }
      }
    }
  }

  std::string generateLogFilenameUnsafe() {
    if (currentLogIndex == 0) {
      int maxIndex = 0;
      if (std::filesystem::exists(logDirectory)) {
        for (const auto& entry :
             std::filesystem::directory_iterator(logDirectory)) {
          std::string filename = entry.path().filename().string();
          if (filename.rfind("quaton.", 0) == 0 && filename != "quaton.log") {
            try {
              size_t dotPos = filename.find('.', 4);
              if (dotPos != std::string::npos) {
                int num = std::stoi(filename.substr(4, dotPos - 4));
                maxIndex = std::max(maxIndex, num);
              }
            } catch (...) {
              continue;
            }
          }
        }
      }
      currentLogIndex = maxIndex + 1;
    }
    return "quaton." + std::to_string(currentLogIndex++) + ".log";
  }

  void exitFileLogger() {
    if (initialized) {
      initialized = false;
      logCondition.notify_all();

      if (logWorkerThread.joinable()) {
        logWorkerThread.join();
      }

      if (logFile.is_open()) {
        logFile.flush();
        logFile.close();
      }
    }
  }

  void exitLogs() {
    if (logFile.is_open()) {
      logFile << "//////////////////// Log End ////////////////////"
              << std::endl;
      logFile << "\n\n\n\n\n";
      logFile.flush();
    }
    saveLog();
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
  std::once_flag workerThreadFlag;
  std::atomic<bool> initialized;
  std::queue<std::string> logQueue;
  std::atomic<int> currentLogIndex;
  LogLevel minLogLevel;
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
