#pragma once

#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "quaton/data_chunk.h"
#include "quaton/downloader/speed_limiter.h"
#include "quaton/http_client.h"
#include "quaton/manifest/manifest_processor.h"

namespace Quaton {

// Forward declarations
class Quaton::ChunkInfo;
class ChunkManifestPair;

// Constants
constexpr int kDefaultMaxParallelism = 8;
constexpr size_t kMd5DigestLength = 16;

/**
 * @class Resource
 * @brief Quaton resource
 *
 * Represents resource files in the Quaton download system
 */
class Resource {
 public:
  // Default constructor
  Resource() = default;

  // Destructor
  ~Resource() = default;

  // Move constructor
  Resource(Resource&& other) noexcept;

  // Move assignment operator
  Resource& operator=(Resource&& other) noexcept;

  // Copy constructor and assignment operator deleted for performance reasons
  Resource(const Resource&) = default;
  Resource& operator=(const Resource&) = default;

  /**
   * @brief Compute difference size
   * @param use_compressed Whether to use compressed size
   * @return Difference size
   */
  long compute_difference_size(bool use_compressed = false) const;

  /**
   * @brief Write resource to stream (sequential download)
   * @param http_client HTTP client instance
   * @param out_stream Output stream
   * @return Asynchronous task
   * @throws std::invalid_argument If http_client or out_stream is empty
   */
  std::future<void> write_to_stream_sequential(
      const std::shared_ptr<HttpClient>& http_client,
      std::shared_ptr<std::ofstream> out_stream);

  /**
   * @brief Write resource to stream (concurrent download)
   * @param http_client HTTP client instance
   * @param out_stream_factory Output stream factory function
   * @param max_parallelism Maximum parallelism
   * @return Asynchronous task
   * @throws std::invalid_argument If http_client or out_stream_factory is empty
   */
  std::future<void> write_to_stream_concurrent(
      const std::shared_ptr<HttpClient>& http_client,
      std::function<std::shared_ptr<std::ofstream>()> out_stream_factory,
      int max_parallelism = 0);

  /**
   * @brief Apply update (from old version to new version)
   * @param http_client HTTP client instance
   * @param old_input_dir Old version input directory
   * @param new_output_dir New version output directory
   * @param chunk_dir Chunk directory
   * @param remove_chunk_after_apply Whether to remove chunk after apply
   * @return Asynchronous task
   * @throws std::invalid_argument If directories are invalid
   */
  std::future<void> apply_update(const std::shared_ptr<HttpClient>& http_client,
                                 const std::string& old_input_dir,
                                 const std::string& new_output_dir,
                                 const std::string& chunk_dir,
                                 bool remove_chunk_after_apply = false);

  /**
   * @brief Download resource using staged workflow
   *
   * Workflow:
   * 1. Download chunks to game_dir/chunks/
   * 2. Validate and assemble in game_dir/staging/
   * 3. Validate complete file MD5
   * 4. Move completed file to game_dir/
   *
   * @param http_client HTTP client instance
   * @param game_dir Game installation directory
   * @param max_parallelism Maximum parallelism
   * @return Asynchronous task
   * @throws std::invalid_argument If game_dir is empty
   */
  std::future<void> download_with_staging_workflow(
      const std::shared_ptr<HttpClient>& http_client,
      const std::string& game_dir,
      int max_parallelism = 0);

  /**
   * @brief Retrieve resources from manifests
   * @param http_client HTTP client instance
   * @param manifest_pair_from Source manifest info pair
   * @param manifest_pair_to Target manifest info pair (for updates)
   * @param matching_field Matching field
   * @param output_dir Output directory to save original and decrypted manifest
   * files (optional)
   * @return Pair of resource list and update size
   * @throws std::runtime_error If manifest processing fails
   */
  static std::pair<std::vector<std::shared_ptr<Resource>>, int64_t>
  retrieve_resources_from_manifests(
      const std::shared_ptr<HttpClient>& http_client,
      const ChunkManifestPair& manifest_pair_from,
      const ChunkManifestPair& manifest_pair_to,
      const std::string& matching_field,
      const std::string& output_dir = "");

  std::string name_;                       ///< Resource name
  int64_t size_;                           ///< Resource size
  std::string hash_;                       ///< Resource hash
  bool is_directory_;                      ///< Whether it's a directory
  bool has_patch_;                         ///< Whether it has patch
  std::vector<Quaton::DataChunk> chunks_;  ///< Chunk list
  std::shared_ptr<SpeedLimiter::State>
      speed_limiter_;  ///< Download speed limiter state
  std::shared_ptr<class Quaton::ChunkInfo> chunks_info_;  ///< Chunk info
  std::shared_ptr<class Quaton::ChunkInfo>
      chunks_info_alt_;  ///< Alternative chunk info

 private:
  /**
   * @brief Execute write operation for single chunk
   * @param http_client HTTP client instance
   * @param out_stream Output stream
   * @param chunk Chunk
   */
  void execute_chunk_write(const std::shared_ptr<HttpClient>& http_client,
                           std::shared_ptr<std::ofstream> out_stream,
                           const Quaton::DataChunk& chunk);

  /**
   * @brief Execute update operation for single chunk
   * @param http_client HTTP client instance
   * @param chunk_dir Chunk directory
   * @param old_file_path Old file path
   * @param new_file_path New file path
   * @param chunk Chunk
   * @param remove_chunk_after_apply Whether to remove chunk after apply
   */
  void execute_chunk_update(const std::shared_ptr<HttpClient>& http_client,
                            const std::string& chunk_dir,
                            const std::string& old_file_path,
                            const std::string& new_file_path,
                            const Quaton::DataChunk& chunk,
                            bool remove_chunk_after_apply);

  /**
   * @brief Struct for downloading and validating single chunk
   */
  struct ChunkPayload {
    std::vector<uint8_t> compressed_payload;    ///< Compressed data
    std::vector<uint8_t> decompressed_payload;  ///< Decompressed data
    std::string md5_checksum;                   ///< MD5 checksum
    std::string chunk_file_path;                ///< Chunk file path
  };

  /**
   * @brief Download single chunk and perform streaming validation
   * @param http_client HTTP client instance
   * @param chunk Chunk
   * @param chunk_dir Chunk directory
   * @return ChunkPayload struct containing data, MD5, etc.
   */
  ChunkPayload fetch_and_validate_chunk(
      const std::shared_ptr<HttpClient>& http_client,
      const Quaton::DataChunk& chunk,
      const std::string& chunk_dir);

  /**
   * @brief Validate and assemble chunks to staging file (using in-memory data)
   * @param staging_file_path Staging file path
   * @param chunk_payloads Chunk payload list
   */
  void merge_chunks_to_staging(const std::string& staging_file_path,
                               const std::vector<ChunkPayload>& chunk_payloads);

  /**
   * @brief Compute file MD5 hash
   * @param file_path File path
   * @return MD5 hash string
   */
  static std::string compute_file_md5(const std::string& file_path);

  /**
   * @brief Compute data MD5 hash
   * @param data Data
   * @return MD5 hash string
   */
  static std::string compute_data_md5(const std::vector<uint8_t>& data);

  /**
   * @brief Process resource
   * @param resource Resource object
   * @param update_size Update size reference
   * @param resources Resource list reference
   */
  static void process_resource(
      const std::shared_ptr<Resource>& resource,
      int64_t& update_size,
      std::vector<std::shared_ptr<Resource>>& resources);

  /**
   * @brief Process update resource
   * @param resource Resource object
   * @param update_size Update size reference
   * @param resources Resource list reference
   */
  static void process_update_resource(
      const std::shared_ptr<Resource>& resource,
      int64_t& update_size,
      std::vector<std::shared_ptr<Resource>>& resources);
};

// Backward compatible type aliases
using Resource = Resource;

}  // namespace Quaton
