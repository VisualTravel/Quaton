#ifndef QUATON_CONFIGURATION_PATH_CONFIG_H
#define QUATON_CONFIGURATION_PATH_CONFIG_H
#pragma once

#include <filesystem>
#include <string>

namespace Quaton {

/**
 * @struct PathConfig
 * @brief Path configuration for Quaton directories
 *
 * Uses std::filesystem::path for all path operations to ensure
 * consistent path separator handling across platforms.
 */
struct PathConfig {
  std::filesystem::path data_dir;      ///< Data directory
  std::filesystem::path db_dir;        ///< Database directory
  std::filesystem::path manifest_dir;  ///< Manifest directory
  std::filesystem::path temp_dir;      ///< Temporary directory
  std::filesystem::path log_dir;       ///< Log directory

  /**
   * @brief Initialize all paths based on base directory
   * @param base_dir Base directory path
   */
  void initialize_from_base(const std::filesystem::path& base_dir) {
    data_dir = base_dir;
    db_dir = base_dir / "quaton_db";
    manifest_dir = base_dir / "manifest";
    temp_dir = base_dir / "temp";
    log_dir = base_dir / "logs";
  }

  /**
   * @brief Create directories if they don't exist
   */
  void ensure_directories_exist() const {
    std::error_code ec;
    if (!data_dir.empty()) {
      std::filesystem::create_directories(data_dir, ec);
    }
    if (!db_dir.empty()) {
      std::filesystem::create_directories(db_dir, ec);
    }
    if (!manifest_dir.empty()) {
      std::filesystem::create_directories(manifest_dir, ec);
    }
    if (!temp_dir.empty()) {
      std::filesystem::create_directories(temp_dir, ec);
    }
    if (!log_dir.empty()) {
      std::filesystem::create_directories(log_dir, ec);
    }
  }

  /**
   * @brief Get data directory as string
   * @return Data directory path as string
   */
  std::string get_data_dir() const { return data_dir.string(); }

  /**
   * @brief Get database directory as string
   * @return Database directory path as string
   */
  std::string get_db_dir() const { return db_dir.string(); }

  /**
   * @brief Get manifest directory as string
   * @return Manifest directory path as string
   */
  std::string get_manifest_dir() const { return manifest_dir.string(); }

  /**
   * @brief Get temp directory as string
   * @return Temp directory path as string
   */
  std::string get_temp_dir() const { return temp_dir.string(); }

  /**
   * @brief Get log directory as string
   * @return Log directory path as string
   */
  std::string get_log_dir() const { return log_dir.string(); }

  /**
   * @brief Create default path configuration
   * @return Default path configuration
   */
  static PathConfig create_default();

  /**
   * @brief Create path configuration with custom base directory
   * @param base_dir Base directory path
   * @return Path configuration with custom base directory
   */
  static PathConfig with_base_directory(const std::filesystem::path& base_dir) {
    PathConfig config;
    config.initialize_from_base(base_dir);
    return config;
  }
};

}  // namespace Quaton

#endif  // QUATON_CONFIGURATION_PATH_CONFIG_H
