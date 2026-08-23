#ifndef QUATON_UTILS_PATH_HELPER_H_
#define QUATON_UTILS_PATH_HELPER_H_
#pragma once

#include <filesystem>
#include <string>

#include "quaton/quaton_global.h"

QUATON_NAMESPACE_BEGIN

/**
 * @class PathHelper
 * @brief Utility class for path-related operations
 *
 * Provides static methods for getting application directories
 * and common path operations.
 */
class QUATON_API PathHelper {
 public:
  /**
   * @brief Get Quaton data directory
   * @return Data directory path
   *
   * On Windows: %APPDATA%\PremiX\Quaton
   * On Linux/macOS: ~/.config/PremiX/Quaton
   */
  static std::string GetDataDir();

  /**
   * @brief Get Quaton database directory
   * @return Database directory path
   */
  static std::string GetDatabaseDir();

  /**
   * @brief Get Quaton manifest directory
   * @return Manifest directory path
   */
  static std::string GetManifestDir();

  /**
   * @brief Get Quaton temporary directory
   * @return Temporary directory path
   */
  static std::string GetTempDir();

  /**
   * @brief Get Quaton logs directory
   * @return Logs directory path
   */
  static std::string GetLogsDir();

  /**
   * @brief Recursively create directory
   * @param path Directory path
   * @return true if creation succeeds or already exists, false otherwise
   */
  static bool CreateDirectoryRecursive(const std::string& path);

  /**
   * @brief Build file path from directory and filename
   * @param directory Directory path
   * @param filename Filename
   * @return Complete file path
   */
  static std::string BuildFilePath(const std::string& directory,
                                   const std::string& filename);

  /**
   * @brief Ensure parent directory exists
   * @param file_path Path to a file
   * @return true if parent directory exists or was created successfully
   */
  static bool EnsureParentDirectory(const std::string& file_path);

  /**
   * @brief Get platform-specific path separator
   * @return Path separator character
   */
  static char GetPathSeparator();

  /**
   * @brief Normalize path separators for current platform
   * @param path Input path
   * @return Normalized path
   */
  static std::string NormalizePath(const std::string& path);

 private:
  PathHelper() = default;  // Static class, no instantiation
};

QUATON_NAMESPACE_END

#endif  // QUATON_UTILS_PATH_HELPER_H_
