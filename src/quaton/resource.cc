#include "quaton/resource.h"

#include <zstd.h>

#include <algorithm>
#include <filesystem>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

#include "quaton/configuration/configuration.h"
#include "quaton/logger.h"
#include "quaton/manifest/chunk_manifest_pair.h"
#include "quaton/url.h"
#include "quaton/utils/checksum.h"
#include "quaton/utils/md5.h"

namespace fs = std::filesystem;

namespace {

// ============================================================================
// Container cache
// ============================================================================
//
// Sophon-style containers aggregate many compressed blocks into one ~10 MiB
// file, so every asset referencing a block inside the same container must
// download that container. The cache below ensures each container URL is
// downloaded at most once per process, while concurrent requests for the same
// URL share a single in-flight download via a shared future.

std::mutex g_container_cache_mutex;
std::unordered_map<std::string, std::shared_future<std::vector<uint8_t>>>
    g_container_cache;  // container URL -> container bytes (once complete)
std::unordered_map<std::string,
                   std::shared_ptr<std::promise<std::vector<uint8_t>>>>
    g_container_pending;  // container URL -> promise held by the leader

// Maximum total cached bytes before the whole cache is dropped.
constexpr size_t kMaxContainerCacheBytes = 512ull * 1024ull * 1024ull;
size_t g_container_cache_bytes = 0;

// Fetches a container, coalescing concurrent downloads for the same URL.
// The first caller downloads the file; all callers share its future.
std::vector<uint8_t> fetch_container(
    const std::shared_ptr<Quaton::HttpClient>& http_client,
    const std::string& url) {
  std::shared_future<std::vector<uint8_t>> future;
  bool is_leader = false;
  {
    std::lock_guard<std::mutex> lock(g_container_cache_mutex);
    if (g_container_cache_bytes > kMaxContainerCacheBytes) {
      g_container_cache.clear();
      g_container_cache_bytes = 0;
    }
    auto it = g_container_cache.find(url);
    if (it != g_container_cache.end()) {
      future = it->second;  // already downloaded or in flight
    } else {
      auto promise = std::make_shared<std::promise<std::vector<uint8_t>>>();
      future = promise->get_future().share();
      g_container_cache.emplace(url, future);
      g_container_pending.emplace(url, promise);
      is_leader = true;
    }
  }

  if (is_leader) {
    try {
      std::vector<uint8_t> data = http_client->get_async(url).get();
      {
        std::lock_guard<std::mutex> lock(g_container_cache_mutex);
        auto it = g_container_pending.find(url);
        if (it != g_container_pending.end()) {
          g_container_cache_bytes += data.size();
          it->second->set_value(std::move(data));
          g_container_pending.erase(it);
        }
      }
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(g_container_cache_mutex);
        auto it = g_container_pending.find(url);
        if (it != g_container_pending.end()) {
          it->second->set_exception(std::current_exception());
          g_container_pending.erase(it);
        }
        g_container_cache.erase(url);
      }
    }
  }

  // All callers (leader included) wait on the shared future.
  return future.get();
}

}  // namespace

namespace Quaton {

// Move constructor
Resource::Resource(Resource&& other) noexcept
    : name_(std::move(other.name_)),
      size_(other.size_),
      hash_(std::move(other.hash_)),
      is_directory_(other.is_directory_),
      has_patch_(other.has_patch_),
      chunks_(std::move(other.chunks_)),
      speed_limiter_(std::move(other.speed_limiter_)),
      chunks_info_(std::move(other.chunks_info_)),
      chunks_info_alt_(std::move(other.chunks_info_alt_)) {
  other.size_ = 0;
  other.is_directory_ = false;
  other.has_patch_ = false;
}

// Move assignment operator
Resource& Resource::operator=(Resource&& other) noexcept {
  if (this != &other) {
    name_ = std::move(other.name_);
    size_ = other.size_;
    hash_ = std::move(other.hash_);
    is_directory_ = other.is_directory_;
    has_patch_ = other.has_patch_;
    chunks_ = std::move(other.chunks_);
    speed_limiter_ = std::move(other.speed_limiter_);
    chunks_info_ = std::move(other.chunks_info_);
    chunks_info_alt_ = std::move(other.chunks_info_alt_);

    // Reset the moved object
    other.size_ = 0;
    other.is_directory_ = false;
    other.has_patch_ = false;
  }
  return *this;
}

long Resource::compute_difference_size(bool use_compressed) const {
  long accumulated_size = 0;
  for (const auto& chunk : chunks_) {
    if (chunk.old_offset_ == -1) {
      accumulated_size +=
          use_compressed ? chunk.size_ : chunk.decompressed_size_;
    }
  }
  return accumulated_size;
}

std::future<void> Resource::write_to_stream_sequential(
    const std::shared_ptr<HttpClient>& http_client,
    std::shared_ptr<std::ofstream> out_stream) {
  if (!http_client) {
    throw std::invalid_argument("HttpClient cannot be null");
  }
  if (!out_stream || !out_stream->is_open()) {
    throw std::invalid_argument("Output stream must be valid and open");
  }

  return std::async(std::launch::async, [this, http_client, out_stream]() {
    try {
      if (chunks_.empty()) {
        throw std::runtime_error("No chunks available for resource: " + name_);
      }

      // Sequentially download each chunk
      for (const auto& chunk : chunks_) {
        execute_chunk_write(http_client, out_stream, chunk);
      }

    } catch (const std::exception& e) {
      LOG_ERROR("Error in write_to_stream_sequential for resource %s: %s",
                name_.c_str(),
                e.what());
      throw;
    }
  });
}

std::future<void> Resource::write_to_stream_concurrent(
    const std::shared_ptr<HttpClient>& http_client,
    std::function<std::shared_ptr<std::ofstream>()> out_stream_factory,
    int max_parallelism) {
  if (!http_client) {
    throw std::invalid_argument("HttpClient cannot be null");
  }
  if (!out_stream_factory) {
    throw std::invalid_argument("Output stream factory cannot be null");
  }

  return std::async(
      std::launch::async,
      [this, http_client, out_stream_factory, max_parallelism]() {
        try {
          if (chunks_.empty()) {
            throw std::runtime_error("No chunks available for resource: " +
                                     name_);
          }

          int concurrency = (max_parallelism > 0)
                                ? max_parallelism
                                : std::thread::hardware_concurrency();
          if (concurrency == 0) concurrency = kDefaultMaxParallelism;

          // Initialize output stream to check validity
          {
            auto test_stream = out_stream_factory();
            if (!test_stream || !test_stream->is_open()) {
              throw std::runtime_error(
                  "Output stream factory failed for resource: " + name_);
            }
          }

          std::vector<std::future<void>> task_list;
          std::mutex task_mutex;

          // Concurrently download chunks
          for (size_t index = 0; index < chunks_.size(); ++index) {
            std::lock_guard<std::mutex> lock(task_mutex);
            if (static_cast<int>(task_list.size()) >= concurrency) {
              // Wait for one task to complete
              task_list.front().get();
              task_list.erase(task_list.begin());
            }

            task_list.emplace_back(std::async(
                std::launch::async,
                [this, http_client, out_stream_factory, index]() {
                  auto stream = out_stream_factory();
                  execute_chunk_write(http_client, stream, chunks_[index]);
                }));
          }

          // Wait for all tasks to complete
          for (auto& task : task_list) {
            task.get();
          }

        } catch (const std::exception& e) {
          LOG_ERROR("Error in write_to_stream_concurrent for resource %s: %s",
                    name_.c_str(),
                    e.what());
          throw;
        }
      });
}

void Resource::execute_chunk_write(
    const std::shared_ptr<HttpClient>& http_client,
    std::shared_ptr<std::ofstream> out_stream,
    const Quaton::DataChunk& chunk) {
  if (!http_client) {
    throw std::invalid_argument("HttpClient cannot be null");
  }
  if (!out_stream || !out_stream->is_open()) {
    throw std::invalid_argument("Output stream must be valid and open");
  }

  try {
    // chunk.name_ is the container file; the block is a slice of it located
    // by container_offset_ with length size_ (Sophon-style).
    std::string chunk_url = Url::BuildChunkUrl(*chunks_info_, chunk.name_);

    std::vector<uint8_t> container_data =
        fetch_container(http_client, chunk_url);

    if (container_data.empty()) {
      throw std::runtime_error("Downloaded container data is empty for: " +
                               chunk.name_);
    }

    // Slice the compressed block out of the container.
    if (chunk.container_offset_ < 0 || chunk.size_ < 0 ||
        chunk.container_offset_ + chunk.size_ >
            static_cast<int64_t>(container_data.size())) {
      throw std::runtime_error(
          "Chunk slice out of container bounds: " + chunk.name_ +
          " offset=" + std::to_string(chunk.container_offset_) +
          " size=" + std::to_string(chunk.size_) +
          " container=" + std::to_string(container_data.size()));
    }
    const uint8_t* compressed_begin =
        container_data.data() + chunk.container_offset_;
    size_t compressed_size = static_cast<size_t>(chunk.size_);

    std::vector<uint8_t> decompressed_data;
    if (chunk.decompressed_size_ != chunk.size_) {
      size_t dstCapacity = static_cast<size_t>(chunk.decompressed_size_);
      decompressed_data.resize(dstCapacity);
      size_t decompressed_size = ZSTD_decompress(decompressed_data.data(),
                                                 dstCapacity,
                                                 compressed_begin,
                                                 compressed_size);

      if (ZSTD_isError(decompressed_size)) {
        throw std::runtime_error(
            "ZSTD decompression failed: " +
            std::string(ZSTD_getErrorName(decompressed_size)));
      }

      decompressed_data.resize(decompressed_size);
    } else {
      decompressed_data.assign(compressed_begin,
                               compressed_begin + compressed_size);
    }

    out_stream->seekp(chunk.offset_);
    if (!out_stream->good()) {
      throw std::runtime_error("Failed to seek output stream for chunk: " +
                               chunk.name_);
    }

    out_stream->write(reinterpret_cast<const char*>(decompressed_data.data()),
                      decompressed_data.size());
    if (!out_stream->good()) {
      throw std::runtime_error(
          "Failed to write data to output stream for chunk: " + chunk.name_);
    }

  } catch (const std::exception& e) {
    LOG_ERROR("Error in execute_chunk_write for chunk %s: %s",
              chunk.name_.c_str(),
              e.what());
    throw;
  }
}

std::future<void> Resource::apply_update(
    const std::shared_ptr<HttpClient>& http_client,
    const std::string& old_input_dir,
    const std::string& new_output_dir,
    const std::string& chunk_dir,
    bool remove_chunk_after_apply) {
  if (!http_client) {
    throw std::invalid_argument("HttpClient cannot be null");
  }
  if (old_input_dir.empty() || new_output_dir.empty() || chunk_dir.empty()) {
    throw std::invalid_argument("Directory paths cannot be empty");
  }

  return std::async(
      std::launch::async,
      [this,
       http_client,
       old_input_dir,
       new_output_dir,
       chunk_dir,
       remove_chunk_after_apply]() {
        try {
          if (chunks_.empty()) {
            throw std::runtime_error("No chunks available for resource: " +
                                     name_);
          }

          if (!fs::exists(old_input_dir)) {
            throw std::runtime_error("Old input directory does not exist: " +
                                     old_input_dir);
          }
          if (!fs::exists(new_output_dir)) {
            throw std::runtime_error("New output directory does not exist: " +
                                     new_output_dir);
          }
          if (!fs::exists(chunk_dir)) {
            throw std::runtime_error("Chunk directory does not exist: " +
                                     chunk_dir);
          }

          const std::string temp_suffix = "_tempUpdate";
          std::string old_file_path =
              (fs::path(old_input_dir) / name_).string();
          std::string new_file_path =
              (fs::path(new_output_dir) / name_).string();
          std::string new_temp_file_path = new_file_path + temp_suffix;
          std::string new_dir_path =
              fs::path(new_file_path).parent_path().string();

          if (!fs::exists(new_dir_path)) {
            fs::create_directories(new_dir_path);
          }

          // Process each chunk
          for (const auto& chunk : chunks_) {
            execute_chunk_update(http_client,
                                 chunk_dir,
                                 old_file_path,
                                 new_temp_file_path,
                                 chunk,
                                 remove_chunk_after_apply);
          }

          // Rename temporary file to final file
          if (fs::exists(new_temp_file_path)) {
            fs::rename(new_temp_file_path, new_file_path);
          }

        } catch (const std::exception& e) {
          LOG_ERROR("Error in apply_update for resource %s: %s",
                    name_.c_str(),
                    e.what());
          throw;
        }
      });
}

void Resource::execute_chunk_update(
    const std::shared_ptr<HttpClient>& http_client,
    const std::string& chunk_dir,
    const std::string& old_file_path,
    const std::string& new_file_path,
    const Quaton::DataChunk& chunk,
    bool remove_chunk_after_apply) {
  if (!http_client) {
    throw std::invalid_argument("HttpClient cannot be null");
  }
  if (chunk_dir.empty()) {
    throw std::invalid_argument("Chunk directory cannot be empty");
  }

  try {
    // chunk.name_ is the container file; download it once into chunk_dir and
    // reuse it for every block that references the same container.
    std::string chunk_path = (fs::path(chunk_dir) / chunk.name_).string();

    if (!fs::exists(chunk_path)) {
      std::string chunk_url = Url::BuildChunkUrl(*chunks_info_, chunk.name_);
      auto data_promise = http_client->get_async(chunk_url);
      auto container_data = data_promise.get();

      {
        std::ofstream chunk_stream(chunk_path, std::ios::binary);
        if (!chunk_stream) {
          throw std::runtime_error("Failed to create chunk file: " +
                                   chunk_path);
        }
        chunk_stream.write(reinterpret_cast<const char*>(container_data.data()),
                           static_cast<std::streamsize>(container_data.size()));
        if (!chunk_stream.good()) {
          throw std::runtime_error("Failed to write chunk file: " + chunk_path);
        }
      }
    }

    std::vector<uint8_t> container_data;
    {
      std::ifstream chunk_stream(chunk_path, std::ios::binary | std::ios::ate);
      if (!chunk_stream) {
        throw std::runtime_error("Failed to open chunk file: " + chunk_path);
      }

      std::streamsize file_size = chunk_stream.tellg();
      if (file_size < 0) {
        throw std::runtime_error("Invalid file size for chunk file: " +
                                 chunk_path);
      }

      chunk_stream.seekg(0, std::ios::beg);
      container_data.resize(static_cast<size_t>(file_size));
      chunk_stream.read(reinterpret_cast<char*>(container_data.data()),
                        file_size);
      if (chunk_stream.gcount() != file_size) {
        throw std::runtime_error("Failed to read complete chunk file: " +
                                 chunk_path);
      }
    }

    // Slice the compressed block out of the container (Sophon-style).
    if (chunk.container_offset_ < 0 || chunk.size_ < 0 ||
        chunk.container_offset_ + chunk.size_ >
            static_cast<int64_t>(container_data.size())) {
      throw std::runtime_error(
          "Chunk slice out of container bounds: " + chunk.name_ +
          " offset=" + std::to_string(chunk.container_offset_) +
          " size=" + std::to_string(chunk.size_) +
          " container=" + std::to_string(container_data.size()));
    }
    const uint8_t* compressed_begin =
        container_data.data() + chunk.container_offset_;
    size_t compressed_size = static_cast<size_t>(chunk.size_);

    std::vector<uint8_t> decompressed_data;
    if (chunk.decompressed_size_ != chunk.size_) {
      size_t dstCapacity = static_cast<size_t>(chunk.decompressed_size_);
      decompressed_data.resize(dstCapacity);
      size_t decompressed_size = ZSTD_decompress(decompressed_data.data(),
                                                 dstCapacity,
                                                 compressed_begin,
                                                 compressed_size);

      if (ZSTD_isError(decompressed_size)) {
        throw std::runtime_error(
            "ZSTD decompression failed: " +
            std::string(ZSTD_getErrorName(decompressed_size)));
      }

      decompressed_data.resize(decompressed_size);
    } else {
      decompressed_data.assign(compressed_begin,
                               compressed_begin + compressed_size);
    }

    {
      std::ofstream new_stream(new_file_path, std::ios::binary | std::ios::app);
      if (!new_stream) {
        throw std::runtime_error("Failed to open new file: " + new_file_path);
      }

      new_stream.seekp(chunk.offset_);
      if (!new_stream.good()) {
        throw std::runtime_error("Failed to seek new file: " + new_file_path);
      }

      new_stream.write(reinterpret_cast<const char*>(decompressed_data.data()),
                       decompressed_data.size());
      if (!new_stream.good()) {
        throw std::runtime_error("Failed to write to new file: " +
                                 new_file_path);
      }

      new_stream.flush();
    }

    if (remove_chunk_after_apply && fs::exists(chunk_path)) {
      fs::remove(chunk_path);
    }

  } catch (const std::exception& e) {
    LOG_ERROR("Error in execute_chunk_update for chunk %s: %s",
              chunk.name_.c_str(),
              e.what());
    throw;
  }
}

std::string Resource::compute_data_md5(const std::vector<uint8_t>& data) {
  return Md5::HashData(data.data(), data.size());
}

std::string Resource::compute_file_md5(const std::string& file_path) {
  if (file_path.empty()) {
    throw std::invalid_argument("File path cannot be empty");
  }
  std::string md5_hash = ChecksumUtils::calculate_md5_file(file_path);
  if (md5_hash.empty()) {
    throw std::runtime_error("Failed to calculate MD5 for file: " + file_path);
  }
  return md5_hash;
}

Resource::ChunkPayload Resource::fetch_and_validate_chunk(
    const std::shared_ptr<HttpClient>& http_client,
    const Quaton::DataChunk& chunk,
    const std::string& chunk_dir) {
  if (!http_client) {
    throw std::invalid_argument("HttpClient cannot be null");
  }
  if (chunk_dir.empty()) {
    throw std::invalid_argument("Chunk directory cannot be empty");
  }

  try {
    ChunkPayload payload;

    if (!fs::exists(chunk_dir)) {
      fs::create_directories(chunk_dir);
    }

    // chunk.name_ is the container file; the block is a slice of it.
    payload.chunk_file_path = (fs::path(chunk_dir) / chunk.name_).string();

    std::stringstream expected_hash_builder;
    for (uint8_t byte : chunk.decompressed_hash_) {
      expected_hash_builder << std::hex << std::setw(2) << std::setfill('0')
                            << static_cast<int>(byte);
    }
    std::string expected_hash = expected_hash_builder.str();

    // Reuse the container file if it already exists locally.
    std::vector<uint8_t> container_data;
    if (fs::exists(payload.chunk_file_path)) {
      std::ifstream existing_file(payload.chunk_file_path,
                                  std::ios::binary | std::ios::ate);
      if (existing_file) {
        std::streamsize file_size = existing_file.tellg();
        existing_file.seekg(0, std::ios::beg);
        container_data.resize(static_cast<size_t>(file_size));
        existing_file.read(reinterpret_cast<char*>(container_data.data()),
                           file_size);
      }
    }

    if (container_data.empty()) {
      container_data = fetch_container(
          http_client, Url::BuildChunkUrl(*chunks_info_, chunk.name_));

      std::ofstream chunk_stream(payload.chunk_file_path, std::ios::binary);
      if (!chunk_stream) {
        throw std::runtime_error("Failed to create chunk file: " +
                                 payload.chunk_file_path);
      }
      chunk_stream.write(reinterpret_cast<const char*>(container_data.data()),
                         static_cast<std::streamsize>(container_data.size()));
      if (!chunk_stream.good()) {
        throw std::runtime_error("Failed to write chunk file: " +
                                 payload.chunk_file_path);
      }
      chunk_stream.close();
    }

    // Slice the compressed block out of the container (Sophon-style).
    if (chunk.container_offset_ < 0 || chunk.size_ < 0 ||
        chunk.container_offset_ + chunk.size_ >
            static_cast<int64_t>(container_data.size())) {
      throw std::runtime_error(
          "Chunk slice out of container bounds: " + chunk.name_ +
          " offset=" + std::to_string(chunk.container_offset_) +
          " size=" + std::to_string(chunk.size_) +
          " container=" + std::to_string(container_data.size()));
    }
    payload.compressed_payload.assign(
        container_data.begin() + chunk.container_offset_,
        container_data.begin() + chunk.container_offset_ + chunk.size_);

    if (payload.compressed_payload.size() != static_cast<size_t>(chunk.size_)) {
      throw std::runtime_error(
          "Chunk size mismatch: expected " + std::to_string(chunk.size_) +
          ", got " + std::to_string(payload.compressed_payload.size()));
    }

    if (chunk.decompressed_size_ != chunk.size_) {
      size_t dstCapacity = static_cast<size_t>(chunk.decompressed_size_);
      payload.decompressed_payload.resize(dstCapacity);
      size_t decompressed_size =
          ZSTD_decompress(payload.decompressed_payload.data(),
                          dstCapacity,
                          payload.compressed_payload.data(),
                          payload.compressed_payload.size());

      if (ZSTD_isError(decompressed_size)) {
        throw std::runtime_error(
            "ZSTD decompression failed: " +
            std::string(ZSTD_getErrorName(decompressed_size)));
      }

      payload.decompressed_payload.resize(decompressed_size);
    } else {
      payload.decompressed_payload = payload.compressed_payload;
    }

    payload.md5_checksum = compute_data_md5(payload.decompressed_payload);

    if (payload.md5_checksum != expected_hash) {
      throw std::runtime_error("Chunk MD5 mismatch for " + chunk.name_ +
                               ": expected " + expected_hash + ", got " +
                               payload.md5_checksum);
    }

    return payload;

  } catch (const std::exception& e) {
    LOG_ERROR("Error fetching chunk %s: %s", chunk.name_.c_str(), e.what());
    throw;
  }
}

void Resource::merge_chunks_to_staging(
    const std::string& staging_file_path,
    const std::vector<ChunkPayload>& chunk_payloads) {
  if (staging_file_path.empty()) {
    throw std::invalid_argument("Staging file path cannot be empty");
  }
  if (chunk_payloads.size() != chunks_.size()) {
    throw std::invalid_argument("ChunkPayload list size mismatch: expected " +
                                std::to_string(chunks_.size()) + ", got " +
                                std::to_string(chunk_payloads.size()));
  }

  try {
    fs::path staging_path(staging_file_path);
    if (!fs::exists(staging_path.parent_path())) {
      fs::create_directories(staging_path.parent_path());
    }

    std::ofstream staging_stream(staging_file_path,
                                 std::ios::binary | std::ios::trunc);
    if (!staging_stream) {
      throw std::runtime_error("Failed to create staging file: " +
                               staging_file_path);
    }

    staging_stream.seekp(size_ - 1);
    staging_stream.write("", 1);
    staging_stream.seekp(0);

    for (size_t index = 0; index < chunks_.size(); ++index) {
      const auto& chunk = chunks_[index];
      const auto& payload = chunk_payloads[index];

      staging_stream.seekp(chunk.offset_);
      if (!staging_stream.good()) {
        throw std::runtime_error("Failed to seek staging file for chunk: " +
                                 chunk.name_);
      }

      staging_stream.write(
          reinterpret_cast<const char*>(payload.decompressed_payload.data()),
          payload.decompressed_payload.size());

      if (!staging_stream.good()) {
        throw std::runtime_error("Failed to write to staging file for chunk: " +
                                 chunk.name_);
      }
    }

    staging_stream.close();

  } catch (const std::exception& e) {
    LOG_ERROR("Error merging chunks for %s: %s", name_.c_str(), e.what());
    throw;
  }
}

std::future<void> Resource::download_with_staging_workflow(
    const std::shared_ptr<HttpClient>& http_client,
    const std::string& game_dir,
    int max_parallelism) {
  if (!http_client) {
    throw std::invalid_argument("HttpClient cannot be null");
  }
  if (game_dir.empty()) {
    throw std::invalid_argument("Game directory cannot be empty");
  }

  return std::async(
      std::launch::async, [this, http_client, game_dir, max_parallelism]() {
        try {
          if (is_directory_) {
            return;
          }

          int concurrency =
              max_parallelism > 0 ? max_parallelism : kDefaultMaxParallelism;

          fs::path game_path(game_dir);
          fs::path chunk_dir_path = game_path / "chunks";
          fs::path staging_dir_path = game_path / "staging";
          fs::path final_path = game_path / name_;

          fs::create_directories(chunk_dir_path);
          fs::create_directories(staging_dir_path);

          std::string chunk_dir_str = chunk_dir_path.string();
          std::string staging_file_path = (staging_dir_path / name_).string();
          std::string final_file_path = final_path.string();

          std::vector<ChunkPayload> payloads;
          payloads.reserve(chunks_.size());

          // Download and validate all chunks
          for (const auto& chunk : chunks_) {
            payloads.push_back(
                fetch_and_validate_chunk(http_client, chunk, chunk_dir_str));
          }

          // Merge chunks to staging area
          merge_chunks_to_staging(staging_file_path, payloads);

          // Verify full file MD5
          std::string actual_md5 = compute_file_md5(staging_file_path);
          if (actual_md5 != hash_) {
            throw std::runtime_error("Full file MD5 mismatch: expected " +
                                     hash_ + ", got " + actual_md5);
          }

          // Move to final location
          if (fs::exists(final_file_path)) {
            fs::remove(final_file_path);
          }
          fs::rename(staging_file_path, final_file_path);

        } catch (const std::exception& e) {
          LOG_ERROR("Error in download_with_staging_workflow for %s: %s",
                    name_.c_str(),
                    e.what());
          throw;
        }
      });
}

// ==================== Resource Static Methods (Original Resources Class)
// ====================

std::pair<std::vector<std::shared_ptr<Resource>>, int64_t>
Resource::retrieve_resources_from_manifests(
    const std::shared_ptr<HttpClient>& http_client,
    const ChunkManifestPair& manifest_pair_from,
    const ChunkManifestPair& manifest_pair_to,
    const std::string& matching_field,
    const std::string& output_dir) {
  if (!http_client) {
    throw std::invalid_argument("HttpClient cannot be null");
  }

  std::vector<std::shared_ptr<Resource>> resources;
  int64_t update_size = 0;

  try {
    // Currently, we only handle full downloads (when manifest URLs are the
    // same) Update operations require more complex logic to compare manifests
    if (manifest_pair_from.get_manifest_metadata().get_base_url() !=
            manifest_pair_to.get_manifest_metadata().get_base_url() ||
        manifest_pair_from.get_chunk_info().get_base_url() !=
            manifest_pair_to.get_chunk_info().get_base_url()) {
      // This is an update operation - currently, only use the target manifest
      LOG_INFO("Update operation detected, using target manifest");
    }

    // Use ManifestProcessor::enumerate_resources to get resources from manifest
    auto manifestResources = ManifestProcessor::enumerate_resources(
        http_client,
        manifest_pair_to.get_manifest_metadata(),
        manifest_pair_to.get_chunk_info(),
        output_dir);

    LOG_INFO("Manifest::EnumerateAsync returned %zu resources",
             manifestResources.size());

    // Process resources - filter out directories and calculate total size
    resources.reserve(manifestResources.size());  // Pre-reserve for performance
    for (const auto& resource : manifestResources) {
      if (resource && !resource->is_directory_) {
        update_size += resource->size_;
        resources.push_back(resource);
        // LOG_DEBUG("Added resource: %s, size: %lld", resource->name_.c_str(),
        // resource->size_);
      } else if (resource) {
        LOG_DEBUG("Skipped directory: %s", resource->name_.c_str());
      }
    }

    LOG_INFO("Retrieved %zu resources from manifest, total size: %llu bytes",
             resources.size(),
             update_size);

  } catch (const std::exception& e) {
    LOG_ERROR("Error in Resource::retrieve_resources_from_manifests: %s",
              e.what());
    throw;
  }

  return {resources, update_size};
}

void Resource::process_resource(
    const std::shared_ptr<Resource>& resource,
    int64_t& update_size,
    std::vector<std::shared_ptr<Resource>>& resources) {
  if (!resource) {
    throw std::invalid_argument("Resource cannot be null");
  }
  if (!resource->is_directory_) {
    update_size += resource->size_;
    resources.push_back(resource);
  }
}

void Resource::process_update_resource(
    const std::shared_ptr<Resource>& resource,
    int64_t& update_size,
    std::vector<std::shared_ptr<Resource>>& resources) {
  if (!resource) {
    throw std::invalid_argument("Resource cannot be null");
  }
  if (!resource->is_directory_) {
    bool is_modified = false;
    for (const auto& chunk : resource->chunks_) {
      if (chunk.old_offset_ == -1) {
        is_modified = true;
        break;
      }
    }

    if (is_modified) {
      update_size += resource->size_;
      resources.push_back(resource);
    }
  }
}

}  // namespace Quaton