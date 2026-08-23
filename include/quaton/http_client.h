#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H
#pragma once

#include <curl/curl.h>
#include <zstd.h>

#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "quaton/configuration/http_config.h"
#include "quaton/quaton_global.h"

namespace Quaton {

class ManifestInfo;

/**
 * @class HttpClient
 * @brief HTTP client class
 *
 * Encapsulates CURL library, provides HTTP request functionality, supports
 * asynchronous requests and streaming data acquisition
 */
class QUATON_API HttpClient {
 public:
  /**
   * @brief Constructor with HttpConfig
   * @param config HTTP configuration
   */
  explicit HttpClient(const HttpConfig& config);

  /**
   * @brief Constructor with max connections (legacy compatibility)
   * @param max_connections_per_server Maximum connections per server, must be
   * greater than 0
   * @throws std::invalid_argument when max_connections_per_server <= 0
   */
  explicit HttpClient(int max_connections_per_server = 128);

  /**
   * @brief Destructor
   */
  ~HttpClient() noexcept;

  /**
   * @brief Asynchronously get data
   * @param url Request URL, cannot be empty
   * @param completion_option Completion option, defaults to
   * "ResponseHeadersRead"
   * @return Future of response data
   * @throws std::invalid_argument when url is empty
   */
  std::future<std::vector<uint8_t>> get_async(
      const std::string& url,
      const std::string& completion_option = "ResponseHeadersRead");

  /**
   * @brief Asynchronous POST request
   * @param url Request URL, cannot be empty
   * @param post_data POST data
   * @param completion_option Completion option, defaults to
   * "ResponseHeadersRead"
   * @return Future of response data
   * @throws std::invalid_argument when url is empty
   */
  std::future<std::vector<uint8_t>> post_async(
      const std::string& url,
      const std::string& post_data,
      const std::string& completion_option = "ResponseHeadersRead");

  /**
   * @brief Asynchronously get stream data
   * @param url Request URL, cannot be empty
   * @param completion_option Completion option, defaults to
   * "ResponseHeadersRead"
   * @return Future of response stream data
   * @throws std::invalid_argument when url is empty
   */
  std::future<std::shared_ptr<std::vector<uint8_t>>> get_stream_async(
      const std::string& url,
      const std::string& completion_option = "ResponseHeadersRead");

  /**
   * @brief Set request headers
   * @param headers List of request headers
   */
  void set_headers(const std::vector<std::string>& headers);

  /**
   * @brief Set timeout
   * @param timeout_ms Timeout in milliseconds, must be greater than 0
   * @throws std::invalid_argument when timeout_ms <= 0
   */
  void set_timeout(long timeout_ms);

  /**
   * @brief Set user agent
   * @param user_agent User agent string, cannot be empty
   * @throws std::invalid_argument when user_agent is empty
   */
  void set_user_agent(const std::string& user_agent);

  /**
   * @brief Read protobuf data from manifest info and save to file
   * @tparam T Protobuf message type
   * @param manifest_info Manifest information
   * @param parser Protobuf parser
   * @param output_dir Output directory, if empty then not saved
   * @return Future of parsed protobuf message
   */
  template <typename T>
  std::future<T> read_proto_from_manifest_info_with_output(
      const ManifestInfo& manifest_info,
      const std::function<T(const std::vector<uint8_t>&)>& parser,
      const std::string& output_dir = "") {
    return std::async(
        std::launch::async, [this, manifest_info, parser, output_dir]() -> T {
          // Get data
          auto dataFuture = get_async(manifest_info.get_file_url());
          auto data = dataFuture.get();

          // If output directory is specified, save original data
          if (!output_dir.empty()) {
            try {
              std::filesystem::path originalDir =
                  std::filesystem::path(output_dir) / "Quaton" / "manifest" /
                  "original";
              std::filesystem::create_directories(originalDir);

              std::string filename = manifest_info.get_file_name() + ".bin";
              std::filesystem::path originalPath = originalDir / filename;

              std::ofstream originalFile(originalPath, std::ios::binary);
              if (originalFile) {
                originalFile.write(reinterpret_cast<const char*>(data.data()),
                                   data.size());
                originalFile.close();
              }
            } catch (const std::exception& e) {
              throw std::runtime_error("Failed to save original manifest: " +
                                       std::string(e.what()));
            }
          }

          std::vector<uint8_t> finalData = data;

          // If compression is used, need to decompress
          if (manifest_info.get_use_compression()) {
            // Zstd decompression
            size_t decompressedSize =
                ZSTD_getFrameContentSize(data.data(), data.size());
            if (decompressedSize == ZSTD_CONTENTSIZE_ERROR) {
              throw std::runtime_error(
                  "Failed to get decompressed size from Zstd frame");
            }
            if (decompressedSize == ZSTD_CONTENTSIZE_UNKNOWN) {
              throw std::runtime_error("Zstd frame content size unknown");
            }
            // Add size validation to avoid buffer overflow
            const size_t maxDecompressedSize =
                100 * 1024 * 1024;  // 100MB limit
            if (decompressedSize > maxDecompressedSize) {
              throw std::runtime_error("Decompressed size too large: " +
                                       std::to_string(decompressedSize) +
                                       " bytes");
            }

            finalData.resize(decompressedSize);
            size_t actualDecompressedSize = ZSTD_decompress(
                finalData.data(), decompressedSize, data.data(), data.size());
            if (ZSTD_isError(actualDecompressedSize)) {
              std::string errorMsg = "Zstd decompression failed: ";
              errorMsg += ZSTD_getErrorName(actualDecompressedSize);
              throw std::runtime_error(errorMsg);
            }

            finalData.resize(actualDecompressedSize);

            // If output directory is specified, save decompressed data
            if (!output_dir.empty()) {
              try {
                std::filesystem::path decompressedDir =
                    std::filesystem::path(output_dir) / "Quaton" / "manifest" /
                    "decompressed";
                std::filesystem::create_directories(decompressedDir);

                std::string filename = manifest_info.get_file_name() + ".bin";
                std::filesystem::path decompressedPath =
                    decompressedDir / filename;

                std::ofstream decompressedFile(decompressedPath,
                                               std::ios::binary);
                if (decompressedFile) {
                  decompressedFile.write(
                      reinterpret_cast<const char*>(finalData.data()),
                      finalData.size());
                  decompressedFile.close();
                }
              } catch (const std::exception& e) {
                throw std::runtime_error(
                    "Failed to save decompressed manifest: " +
                    std::string(e.what()));
              }
            }
          }

          // Parse protobuf
          return parser(finalData);
        });
  }

 private:
  /**
   * @brief Perform asynchronous HTTP request
   * @param url Request URL
   * @param setup_options Function to setup options
   * @return Future of response data
   */
  std::future<std::vector<uint8_t>> perform_async_request(
      const std::string& url, const std::function<void(CURL*)>& setup_options);

  /**
   * @brief Perform asynchronous stream HTTP request
   * @param url Request URL
   * @param setup_options Function to setup options
   * @return Future of response stream data
   */
  std::future<std::shared_ptr<std::vector<uint8_t>>>
  perform_async_stream_request(const std::string& url,
                               const std::function<void(CURL*)>& setup_options);

  /**
   * @brief Copy request headers list
   * @return Copied request headers list, throws exception on failure
   */
  curl_slist* copy_headers();

  /**
   * @brief Set CURL options
   * @param handle CURL handle
   * @param timeout Timeout
   * @param user_agent User agent
   * @param max_connections Maximum connections
   * @param headers_copy Request headers copy
   */
  void setup_curl_options(CURL* handle,
                          long timeout,
                          const std::string& user_agent,
                          int max_connections,
                          curl_slist* headers_copy);

  /**
   * @brief CURL write callback function
   * @param contents Data contents
   * @param size Size of data block
   * @param nmemb Number of data blocks
   * @param user_data User data pointer
   * @return Number of bytes written
   */
  static size_t write_callback(void* contents,
                               size_t size,
                               size_t nmemb,
                               void* user_data);

  CURL* curl_;                      ///< CURL handle
  curl_slist* headers_;             ///< Request headers list
  long timeout_ms_;                 ///< Timeout
  std::string user_agent_;          ///< User agent
  int max_connections_per_server_;  ///< Maximum connections per server
  bool follow_redirects_;           ///< Follow HTTP redirects
  bool ssl_verify_peer_;            ///< Verify SSL peer certificate
  bool ssl_verify_host_;            ///< Verify SSL host
  std::mutex config_mutex_;         ///< Configuration mutex
};

}  // namespace Quaton

#endif  // HTTP_CLIENT_H