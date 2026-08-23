#ifndef DOWNLOADER_UTILS_H
#define DOWNLOADER_UTILS_H
#pragma once

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "quaton/quaton_global.h"
#include "quaton/utils/retry_helper.h"

QUATON_NAMESPACE_BEGIN

// Forward declarations
class HttpClient;

// ============================================================================
// DownloadUtils - Download Utility Class
// ============================================================================

/**
 * @class DownloadUtils
 * @brief Collection of public utility functions related to download
 *
 * Provides common functions such as download, verification, URL building, etc.,
 * to avoid code duplication
 */
class DownloadUtils {
 public:
  /**
   * @struct DownloadFileResult
   * @brief File download result
   */
  struct DownloadFileResult {
    bool success;               ///< whether successful
    std::vector<uint8_t> data;  ///< downloaded data
    std::string error_message;  ///< error message

    DownloadFileResult() : success(false) {}
  };

  /**
   * @struct FileVerificationResult
   * @brief File verification result
   */
  struct FileVerificationResult {
    bool size_match;            ///< whether size matches
    bool md5_match;             ///< whether MD5 matches
    std::string actual_md5;     ///< actual MD5 value
    std::string error_message;  ///< error message

    FileVerificationResult() : size_match(false), md5_match(false) {}

    bool is_valid() const { return size_match && md5_match; }
  };

  /**
   * @brief Build download URL
   * @param url_prefix URL prefix
   * @param file_identifier file identifier
   * @param url_suffix URL suffix (optional)
   * @return complete download URL
   */
  static std::string BuildDownloadUrl(const std::string& url_prefix,
                                      const std::string& file_identifier,
                                      const std::string& url_suffix = "");

  /**
   * @brief Build file save path
   * @param directory directory path
   * @param filename filename
   * @return complete file path
   */
  static std::string BuildFilePath(const std::string& directory,
                                   const std::string& filename);

  /**
   * @brief Download file to memory
   * @param http_client HTTP client
   * @param url download URL
   * @param expected_size expected file size (0 means no check)
   * @param expected_md5 expected MD5 value (empty means no check)
   * @return download result
   */
  static DownloadFileResult DownloadToMemory(
      const std::shared_ptr<HttpClient>& http_client,
      const std::string& url,
      int64_t expected_size = 0,
      const std::string& expected_md5 = "");

  /**
   * @brief Download file and save to disk
   * @param http_client HTTP client
   * @param url download URL
   * @param save_path save path
   * @param expected_size expected file size (0 means no check)
   * @param expected_md5 expected MD5 value (empty means no check)
   * @return whether successful
   */
  static bool DownloadToFile(const std::shared_ptr<HttpClient>& http_client,
                             const std::string& url,
                             const std::string& save_path,
                             int64_t expected_size = 0,
                             const std::string& expected_md5 = "");

  /**
   * @brief Verify downloaded file
   * @param file_path file path
   * @param expected_size expected file size (0 means no check)
   * @param expected_md5 expected MD5 value (empty means no check)
   * @return verification result
   */
  static FileVerificationResult VerifyDownloadedFile(
      const std::string& file_path,
      int64_t expected_size = 0,
      const std::string& expected_md5 = "");

  /**
   * @brief Verify data in memory
   * @param data data pointer
   * @param data_size data size
   * @param expected_size expected size (0 means no check)
   * @param expected_md5 expected MD5 value (empty means no check)
   * @return verification result
   */
  static FileVerificationResult VerifyMemoryData(
      const uint8_t* data,
      size_t data_size,
      int64_t expected_size = 0,
      const std::string& expected_md5 = "");

  /**
   * @brief Save data to file
   * @param data data pointer
   * @param data_size data size
   * @param save_path save path
   * @return whether successful
   */
  static bool SaveDataToFile(const uint8_t* data,
                             size_t data_size,
                             const std::string& save_path);

  /**
   * @brief Check if file exists and is valid
   * @param file_path file path
   * @param expected_size expected file size (0 means no check)
   * @param expected_md5 expected MD5 value (empty means no check)
   * @return whether exists and valid
   */
  static bool IsFileExistAndValid(const std::string& file_path,
                                  int64_t expected_size = 0,
                                  const std::string& expected_md5 = "");

  /**
   * @brief Extract ZIP file to specified directory
   * @param zip_path ZIP file path
   * @param extract_dir extract target directory
   * @return success returns true, failure returns false
   */
  static bool ExtractZip(const std::string& zip_path,
                         const std::string& extract_dir);

  /**
   * @brief Read JSON object file line by line (each line is a JSON object),
   * parse into JSON list
   * @param file_path file path
   * @return successfully parsed JSON object list
   */
  static std::vector<nlohmann::json> ReadJsonLines(
      const std::string& file_path);
};

QUATON_NAMESPACE_END

#endif  // DOWNLOADER_UTILS_H
