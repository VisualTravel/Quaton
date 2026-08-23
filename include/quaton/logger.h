#ifndef LOGGER_H
#define LOGGER_H
#pragma once

#include <memory>
#include <string>

#include "quaton/quaton_global.h"  // For QUATON_API macro

// Platform detection
#if defined(_WIN32)
#define OS_WINDOWS
#elif defined(__APPLE__)
#define OS_MACOS
#elif defined(__linux__)
#define OS_LINUX
#endif

/**
 * @brief Log trace level information
 * @param fmt Format string
 * @param ... Variable argument list
 */
#define LOG_TRACE(fmt, ...) \
  Logger::log(LogLevel::Trace, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/**
 * @brief Log debug level information
 * @param fmt Format string
 * @param ... Variable argument list
 */
#define LOG_DEBUG(fmt, ...) \
  Logger::log(LogLevel::Debug, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/**
 * @brief Log info level information
 * @param fmt Format string
 * @param ... Variable argument list
 */
#define LOG_INFO(fmt, ...) \
  Logger::log(LogLevel::Info, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/**
 * @brief Log warning level information
 * @param fmt Format string
 * @param ... Variable argument list
 */
#define LOG_WARN(fmt, ...) \
  Logger::log(LogLevel::Warn, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/**
 * @brief Log error level information
 * @param fmt Format string
 * @param ... Variable argument list
 */
#define LOG_ERROR(fmt, ...) \
  Logger::log(LogLevel::Error, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/**
 * @brief Log fatal level information
 * @param fmt Format string
 * @param ... Variable argument list
 */
#define LOG_FATAL(fmt, ...) \
  Logger::log(LogLevel::Fatal, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

// Conditional logging macros with silent flag
#define LOG_INFO_VERBOSE(silent, fmt, ...)       \
  do {                                           \
    if (!(silent)) LOG_INFO(fmt, ##__VA_ARGS__); \
  } while (0)

#define LOG_WARN_VERBOSE(silent, fmt, ...)       \
  do {                                           \
    if (!(silent)) LOG_WARN(fmt, ##__VA_ARGS__); \
  } while (0)

#define LOG_ERROR_VERBOSE(silent, fmt, ...)       \
  do {                                            \
    if (!(silent)) LOG_ERROR(fmt, ##__VA_ARGS__); \
  } while (0)

/**
 * @brief Log level enumeration
 *
 * Defines different levels of the logging system and their corresponding
 * priorities
 */
enum class LogLevel : int {
  Trace = 0,  ///< Trace level, most detailed log information
  Debug = 1,  ///< Debug level, detailed information during debugging
  Info = 2,   ///< Info level, general informational messages
  Warn = 3,   ///< Warning level, potential error information
  Error = 4,  ///< Error level, error information
  Fatal = 5   ///< Fatal level, fatal errors that cause program termination
};

/**
 * @class Logger
 * @brief Asynchronous logging class
 *
 * Provides asynchronous file logging functionality, supports thread-safe log
 * conversion
 */
class QUATON_API Logger {
 public:
  /**
   * @brief Initialize logging system
   * @return Whether initialization succeeded
   */
  static bool initialize();

  /**
   * @brief Log information
   * @param level Log level
   * @param file Source file name
   * @param line Line number
   * @param fmt Format string
   * @param ... Variable argument list
   */
  static void log(
      LogLevel level, const char* file, int line, const char* fmt, ...);

  /**
   * @brief Save log file
   */
  static void saveLog();

  /**
   * @brief Exit all logging resources
   */
  static void exitLogs();

  /**
   * @brief Set log level threshold
   * @param level Log level to set
   */
  static void setLogLevel(LogLevel level);

 private:
  // Delete default constructor and destructor to prevent creating instances of
  // static class
  Logger() = delete;
  ~Logger() = delete;
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  // PIMPL private implementation class, forward declaration
  class LoggerImpl;
  static LoggerImpl* pImpl;
};

#endif  // LOGGER_H
