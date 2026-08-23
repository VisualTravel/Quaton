#include "quaton/downloader/downloader_utils.h"

#include <zip.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "quaton/http_client.h"
#include "quaton/logger.h"
#include "quaton/utils/checksum.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace Quaton {

// TaskQueue implementation has been moved to base/task_queue.cc
// RetryHelper is a header-only template in utils/retry_helper.h

std::string DownloadUtils::BuildDownloadUrl(const std::string& url_prefix,
                                            const std::string& file_identifier,
                                            const std::string& url_suffix) {
  std::string url = url_prefix;

  if (!url.empty() && url.back() != '/') {
    url += "/";
  }

  url += file_identifier;

  if (!url_suffix.empty()) {
    url += url_suffix;
  }

  return url;
}

std::string DownloadUtils::BuildFilePath(const std::string& directory,
                                         const std::string& filename) {
  std::string path = directory;

  if (!path.empty() && path.back() != '/' && path.back() != '\\') {
#ifdef _WIN32
    path += "\\";
#else
    path += "/";
#endif
  }

  path += filename;

  return path;
}

DownloadUtils::DownloadFileResult DownloadUtils::DownloadToMemory(
    const std::shared_ptr<HttpClient>& http_client,
    const std::string& url,
    int64_t expected_size,
    const std::string& expected_md5) {
  DownloadFileResult result;

  try {
    result.data = http_client->get_async(url).get();

    if (result.data.empty()) {
      result.error_message = "Empty response from server";
      return result;
    }

    auto verify_result = VerifyMemoryData(
        result.data.data(), result.data.size(), expected_size, expected_md5);

    if (!verify_result.is_valid()) {
      result.error_message = verify_result.error_message;
      result.data.clear();
      return result;
    }

    result.success = true;
    return result;

  } catch (const std::exception& e) {
    result.success = false;
    result.error_message = std::string("Exception: ") + e.what();
    result.data.clear();
    return result;
  }
}

bool DownloadUtils::DownloadToFile(
    const std::shared_ptr<HttpClient>& http_client,
    const std::string& url,
    const std::string& save_path,
    int64_t expected_size,
    const std::string& expected_md5) {
  try {
    auto download_result =
        DownloadToMemory(http_client, url, expected_size, expected_md5);

    if (!download_result.success) {
      LOG_ERROR("Failed to download file: %s",
                download_result.error_message.c_str());
      return false;
    }

    if (!SaveDataToFile(download_result.data.data(),
                        download_result.data.size(),
                        save_path)) {
      LOG_ERROR("Failed to save file: %s", save_path.c_str());
      return false;
    }

    return true;

  } catch (const std::exception& e) {
    LOG_ERROR("Exception in DownloadToFile: %s", e.what());
    return false;
  }
}

DownloadUtils::FileVerificationResult DownloadUtils::VerifyDownloadedFile(
    const std::string& file_path,
    int64_t expected_size,
    const std::string& expected_md5) {
  FileVerificationResult result;

  try {
    if (!fs::exists(file_path)) {
      result.error_message = "File does not exist";
      return result;
    }

    auto file_size = fs::file_size(file_path);

    if (expected_size > 0) {
      result.size_match = (file_size == static_cast<uintmax_t>(expected_size));
      if (!result.size_match) {
        result.error_message = "Size mismatch: expected " +
                               std::to_string(expected_size) + ", got " +
                               std::to_string(file_size);
        return result;
      }
    } else {
      result.size_match = true;
    }

    if (!expected_md5.empty()) {
      std::ifstream file(file_path, std::ios::binary | std::ios::ate);
      if (!file.is_open()) {
        result.error_message = "Failed to open file for MD5 verification";
        return result;
      }

      std::vector<uint8_t> buffer(file_size);
      file.seekg(0, std::ios::beg);
      file.read(reinterpret_cast<char*>(buffer.data()), file_size);
      file.close();

      result.actual_md5 =
          ChecksumUtils::calculate_md5_data(buffer.data(), buffer.size());
      result.md5_match = (result.actual_md5 == expected_md5);

      if (!result.md5_match) {
        result.error_message = "MD5 mismatch: expected " + expected_md5 +
                               ", got " + result.actual_md5;
        return result;
      }
    } else {
      result.md5_match = true;  // Considered matching when MD5 is not checked
    }

    return result;

  } catch (const std::exception& e) {
    result.error_message = std::string("Exception: ") + e.what();
    return result;
  }
}

DownloadUtils::FileVerificationResult DownloadUtils::VerifyMemoryData(
    const uint8_t* data,
    size_t data_size,
    int64_t expected_size,
    const std::string& expected_md5) {
  FileVerificationResult result;

  try {
    if (data == nullptr || data_size == 0) {
      result.error_message = "Invalid data pointer or size";
      return result;
    }

    if (expected_size > 0) {
      result.size_match = (static_cast<int64_t>(data_size) == expected_size);
      if (!result.size_match) {
        result.error_message = "Size mismatch: expected " +
                               std::to_string(expected_size) + ", got " +
                               std::to_string(data_size);
        return result;
      }
    } else {
      result.size_match = true;
    }

    if (!expected_md5.empty()) {
      result.actual_md5 = ChecksumUtils::calculate_md5_data(data, data_size);
      result.md5_match = (result.actual_md5 == expected_md5);

      if (!result.md5_match) {
        result.error_message = "MD5 mismatch: expected " + expected_md5 +
                               ", got " + result.actual_md5;
        return result;
      }
    } else {
      result.md5_match = true;
    }

    return result;

  } catch (const std::exception& e) {
    result.error_message = std::string("Exception: ") + e.what();
    return result;
  }
}

bool DownloadUtils::SaveDataToFile(const uint8_t* data,
                                   size_t data_size,
                                   const std::string& save_path) {
  try {
    fs::path file_path(save_path);
    if (file_path.has_parent_path()) {
      std::error_code ec;
      fs::create_directories(file_path.parent_path(), ec);
      if (ec) {
        LOG_ERROR("Failed to create directory: %s", ec.message().c_str());
        return false;
      }
    }

    std::ofstream out_file(save_path, std::ios::binary | std::ios::trunc);
    if (!out_file.is_open()) {
      LOG_ERROR("Failed to open file for writing: %s", save_path.c_str());
      return false;
    }

    out_file.write(reinterpret_cast<const char*>(data), data_size);
    out_file.close();

    if (!out_file.good()) {
      LOG_ERROR("Failed to write data to file: %s", save_path.c_str());
      return false;
    }

    return true;

  } catch (const std::exception& e) {
    LOG_ERROR("Exception in SaveDataToFile: %s", e.what());
    return false;
  }
}

bool DownloadUtils::ExtractZip(const std::string& zip_path,
                               const std::string& extract_dir) {
  try {
    LOG_INFO("Extracting ZIP file: %s", zip_path.c_str());
    std::error_code ec;
    fs::create_directories(extract_dir, ec);
    if (ec) {
      LOG_ERROR("Failed to create extract directory: %s", ec.message().c_str());
      return false;
    }

    int err = 0;
    zip_t* archive = zip_open(zip_path.c_str(), ZIP_RDONLY, &err);
    if (!archive) {
      zip_error_t error;
      zip_error_init_with_code(&error, err);
      LOG_ERROR("Failed to open ZIP file: %s", zip_error_strerror(&error));
      zip_error_fini(&error);
      return false;
    }

    zip_int64_t num_entries = zip_get_num_entries(archive, 0);
    if (num_entries < 0) {
      LOG_ERROR("Failed to get number of entries in ZIP");
      zip_close(archive);
      return false;
    }

    for (zip_int64_t i = 0; i < num_entries; ++i) {
      const char* name = zip_get_name(archive, i, 0);
      if (!name) continue;

      fs::path out_path = fs::path(extract_dir) / name;

      // directory
      if (name[strlen(name) - 1] == '/' || name[strlen(name) - 1] == '\\') {
        fs::create_directories(out_path);
        continue;
      }

      fs::create_directories(out_path.parent_path());

      zip_file_t* zf = zip_fopen_index(archive, i, 0);
      if (!zf) {
        LOG_WARN("Failed to open entry in ZIP: %s", name);
        continue;
      }

      std::ofstream out_file(out_path, std::ios::binary);
      if (!out_file) {
        LOG_WARN("Failed to create output file: %s", out_path.string().c_str());
        zip_fclose(zf);
        continue;
      }

      char buffer[8192];
      zip_int64_t bytes_read;
      while ((bytes_read = zip_fread(zf, buffer, sizeof(buffer))) > 0) {
        out_file.write(buffer, bytes_read);
      }

      out_file.close();
      zip_fclose(zf);
    }

    zip_close(archive);
    LOG_INFO("ZIP extraction completed: %s", zip_path.c_str());
    return true;

  } catch (const std::exception& e) {
    LOG_ERROR("Exception in ExtractZip: %s", e.what());
    return false;
  }
}

std::vector<nlohmann::json> DownloadUtils::ReadJsonLines(
    const std::string& file_path) {
  std::vector<nlohmann::json> result;
  try {
    std::ifstream in(file_path);
    if (!in.is_open()) {
      LOG_ERROR("Failed to open JSON-lines file: %s", file_path.c_str());
      return result;
    }

    std::string line;
    size_t line_no = 0;
    while (std::getline(in, line)) {
      ++line_no;
      if (line.empty() ||
          line.find_first_not_of(" \t\r\n") == std::string::npos)
        continue;
      try {
        nlohmann::json j = nlohmann::json::parse(line);
        result.push_back(std::move(j));
      } catch (const nlohmann::json::exception& e) {
        LOG_WARN("Failed to parse JSON line %zu in %s: %s",
                 line_no,
                 file_path.c_str(),
                 e.what());
        continue;
      }
    }

  } catch (const std::exception& e) {
    LOG_ERROR("Exception in ReadJsonLines: %s", e.what());
  }
  return result;
}

bool DownloadUtils::IsFileExistAndValid(const std::string& file_path,
                                        int64_t expected_size,
                                        const std::string& expected_md5) {
  auto result = VerifyDownloadedFile(file_path, expected_size, expected_md5);
  return result.is_valid();
}

}  // namespace Quaton
