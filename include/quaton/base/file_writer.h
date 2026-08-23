#ifndef QUATON_BASE_FILE_WRITER_H
#define QUATON_BASE_FILE_WRITER_H
#pragma once

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Quaton {
namespace FileWriter {

/**
 * @brief Create directories recursively (like mkdir -p)
 * @param path Directory path to create
 * @param ec Optional error code output
 * @return true if successful or directory already exists, false otherwise
 */
bool create_directories(const std::filesystem::path& path,
                        std::error_code& ec) noexcept;

/**
 * @brief Create directories recursively (like mkdir -p)
 * @param path Directory path to create
 * @return true if successful or directory already exists, false otherwise
 */
bool create_directories(const std::filesystem::path& path);

/**
 * @brief Write data to a file (binary mode with truncate)
 * @param file_path Path to the output file
 * @param data Pointer to the data buffer
 * @param data_size Size of data in bytes
 * @return true if successful, false otherwise
 */
bool write_binary_file(const std::string& file_path,
                       const uint8_t* data,
                       size_t data_size);

/**
 * @brief Write data to a file (binary mode with truncate)
 * @param file_path Path to the output file
 * @param data Vector containing the data
 * @return true if successful, false otherwise
 */
bool write_binary_file(const std::string& file_path,
                       const std::vector<uint8_t>& data);

/**
 * @brief Write string to a file (text mode)
 * @param file_path Path to the output file
 * @param content String content to write
 * @return true if successful, false otherwise
 */
bool write_text_file(const std::string& file_path, const std::string& content);

/**
 * @brief Open a binary file stream for writing
 * @param file_path Path to the output file
 * @param mode Open mode flags (default: binary | out | trunc)
 * @return Shared pointer to the output file stream, or nullptr on failure
 */
std::shared_ptr<std::ofstream> open_binary_stream(
    const std::string& file_path,
    std::ios_base::openmode mode = std::ios::binary | std::ios::out |
                                   std::ios::trunc);

/**
 * @brief Open a binary file stream for appending
 * @param file_path Path to the output file
 * @return Shared pointer to the output file stream, or nullptr on failure
 */
std::shared_ptr<std::ofstream> open_append_stream(const std::string& file_path);

/**
 * @brief Create a file stream factory function
 * @param file_path Path to the output file
 * @param mode Open mode flags
 * @return Factory function that creates a new file stream
 */
std::function<std::shared_ptr<std::ofstream>()> create_stream_factory(
    const std::string& file_path,
    std::ios_base::openmode mode = std::ios::binary | std::ios::out |
                                   std::ios::trunc);

/**
 * @brief Ensure parent directory exists and is writable
 * @param file_path Path to a file
 * @return true if parent directory exists or was created successfully
 */
bool ensure_parent_directory(const std::filesystem::path& file_path);

}  // namespace FileWriter
}  // namespace Quaton

#endif  // QUATON_BASE_FILE_WRITER_H
