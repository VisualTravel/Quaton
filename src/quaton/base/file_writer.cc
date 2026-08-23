#include "quaton/base/file_writer.h"

#include <filesystem>
#include <fstream>
#include <memory>

#include "quaton/logger.h"

namespace Quaton {
namespace FileWriter {

namespace fs = std::filesystem;

bool create_directories(const fs::path& path, std::error_code& ec) noexcept {
  try {
    if (path.empty()) {
      return false;
    }

    if (fs::exists(path)) {
      return fs::is_directory(path);
    }

    return fs::create_directories(path, ec);
  } catch (const std::exception& e) {
    LOG_ERROR("Exception in create_directories: %s", e.what());
    return false;
  }
}

bool create_directories(const fs::path& path) {
  std::error_code ec;
  fs::create_directories(path, ec);
  if (ec && !fs::exists(path)) {
    LOG_ERROR("Failed to create directories '%s': %s",
              path.string().c_str(),
              ec.message().c_str());
    return false;
  }
  return true;
}

bool write_binary_file(const std::string& file_path,
                       const uint8_t* data,
                       size_t data_size) {
  if (!data || data_size == 0) {
    LOG_ERROR("Invalid data provided to write_binary_file");
    return false;
  }

  try {
    fs::path path(file_path);

    if (path.has_parent_path() && !path.parent_path().empty()) {
      std::error_code ec;
      fs::create_directories(path.parent_path(), ec);
      if (ec && !fs::exists(path.parent_path())) {
        LOG_ERROR("Failed to create parent directory for '%s': %s",
                  file_path.c_str(),
                  ec.message().c_str());
        return false;
      }
    }

    std::ofstream out_file(file_path, std::ios::binary | std::ios::trunc);
    if (!out_file.is_open()) {
      LOG_ERROR("Failed to open file for writing: %s", file_path.c_str());
      return false;
    }

    out_file.write(reinterpret_cast<const char*>(data), data_size);
    out_file.close();

    if (!out_file.good()) {
      LOG_ERROR("Failed to write data to file: %s", file_path.c_str());
      return false;
    }

    LOG_DEBUG("Successfully wrote %zu bytes to file: %s",
              data_size,
              file_path.c_str());
    return true;

  } catch (const std::exception& e) {
    LOG_ERROR("Exception in write_binary_file: %s", e.what());
    return false;
  }
}

bool write_binary_file(const std::string& file_path,
                       const std::vector<uint8_t>& data) {
  return write_binary_file(file_path, data.data(), data.size());
}

bool write_text_file(const std::string& file_path, const std::string& content) {
  try {
    fs::path path(file_path);

    if (path.has_parent_path() && !path.parent_path().empty()) {
      std::error_code ec;
      fs::create_directories(path.parent_path(), ec);
      if (ec && !fs::exists(path.parent_path())) {
        LOG_ERROR("Failed to create parent directory for '%s': %s",
                  file_path.c_str(),
                  ec.message().c_str());
        return false;
      }
    }

    std::ofstream out_file(file_path);
    if (!out_file.is_open()) {
      LOG_ERROR("Failed to open file for writing: %s", file_path.c_str());
      return false;
    }

    out_file << content;
    out_file.close();

    if (!out_file.good()) {
      LOG_ERROR("Failed to write text to file: %s", file_path.c_str());
      return false;
    }

    LOG_DEBUG("Successfully wrote text to file: %s", file_path.c_str());
    return true;

  } catch (const std::exception& e) {
    LOG_ERROR("Exception in write_text_file: %s", e.what());
    return false;
  }
}

std::shared_ptr<std::ofstream> open_binary_stream(
    const std::string& file_path, std::ios_base::openmode mode) {
  try {
    fs::path path(file_path);

    if (path.has_parent_path() && !path.parent_path().empty()) {
      std::error_code ec;
      fs::create_directories(path.parent_path(), ec);
      if (ec && !fs::exists(path.parent_path())) {
        LOG_ERROR("Failed to create parent directory for '%s': %s",
                  file_path.c_str(),
                  ec.message().c_str());
        return nullptr;
      }
    }

    auto stream = std::make_shared<std::ofstream>();
    stream->open(file_path, mode);

    if (!stream->is_open()) {
      LOG_ERROR("Failed to open file stream: %s", file_path.c_str());
      return nullptr;
    }

    LOG_DEBUG("Successfully opened file stream: %s", file_path.c_str());
    return stream;

  } catch (const std::exception& e) {
    LOG_ERROR("Exception in open_binary_stream: %s", e.what());
    return nullptr;
  }
}

std::shared_ptr<std::ofstream> open_append_stream(
    const std::string& file_path) {
  return open_binary_stream(file_path,
                            std::ios::binary | std::ios::out | std::ios::app);
}

std::function<std::shared_ptr<std::ofstream>()> create_stream_factory(
    const std::string& file_path, std::ios_base::openmode mode) {
  return [file_path, mode]() -> std::shared_ptr<std::ofstream> {
    return open_binary_stream(file_path, mode);
  };
}

bool ensure_parent_directory(const fs::path& file_path) {
  if (!file_path.has_parent_path()) {
    return true;  // No parent directory needed
  }

  fs::path parent = file_path.parent_path();
  if (parent.empty()) {
    return true;  // Current directory
  }

  std::error_code ec;
  fs::create_directories(parent, ec);
  return !ec || fs::exists(parent);
}

}  // namespace FileWriter
}  // namespace Quaton
