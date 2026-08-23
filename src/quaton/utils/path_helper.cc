#include "quaton/utils/path_helper.h"

#include <cerrno>
#include <cstdlib>

#include "quaton/logger.h"

#ifdef QUATON_PLATFORM_WINDOWS
#include <direct.h>
#define platform_mkdir(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define platform_mkdir(path) mkdir(path, 0755)
#endif

QUATON_NAMESPACE_BEGIN

std::string PathHelper::GetDataDir() {
#ifdef QUATON_PLATFORM_WINDOWS
  const char* app_data = std::getenv("APPDATA");
  if (app_data) {
    return std::string(app_data) + "\\PremiX\\Quaton";
  }
  return "PremiX\\Quaton";
#else
  const char* home = std::getenv("HOME");
  if (home) {
    return std::string(home) + "/.config/PremiX/Quaton";
  }
  return "PremiX/Quaton";
#endif
}

std::string PathHelper::GetDatabaseDir() {
#ifdef QUATON_PLATFORM_WINDOWS
  return GetDataDir() + "\\quaton_db";
#else
  return GetDataDir() + "/quaton_db";
#endif
}

std::string PathHelper::GetManifestDir() {
#ifdef QUATON_PLATFORM_WINDOWS
  return GetDataDir() + "\\manifest";
#else
  return GetDataDir() + "/manifest";
#endif
}

std::string PathHelper::GetTempDir() {
#ifdef QUATON_PLATFORM_WINDOWS
  return GetDataDir() + "\\temp";
#else
  return GetDataDir() + "/temp";
#endif
}

std::string PathHelper::GetLogsDir() {
#ifdef QUATON_PLATFORM_WINDOWS
  return GetDataDir() + "\\logs";
#else
  return GetDataDir() + "/logs";
#endif
}

bool PathHelper::CreateDirectoryRecursive(const std::string& path) {
  if (path.empty()) {
    return false;
  }

  std::string current_path;
  size_t pos = 0;

#ifdef QUATON_PLATFORM_WINDOWS
  if (path.length() >= 2 && path[1] == ':') {
    pos = 2;
    if (path.length() > 2 && (path[2] == '\\' || path[2] == '/')) {
      pos = 3;
    }
    current_path = path.substr(0, pos);
  }
  const char separator = '\\';
#else
  if (path[0] == '/') {
    pos = 1;
    current_path = "/";
  }
  const char separator = '/';
#endif

  while (pos < path.length()) {
    size_t next_pos = path.find_first_of("\\/", pos);
    if (next_pos == std::string::npos) {
      next_pos = path.length();
    }

    if (!current_path.empty() && current_path.back() != separator) {
      current_path += separator;
    }
    current_path += path.substr(pos, next_pos - pos);

    if (platform_mkdir(current_path.c_str()) != 0) {
      if (errno != EEXIST) {
        LOG_ERROR("Failed to create directory: %s, errno: %d",
                  current_path.c_str(),
                  errno);
        return false;
      }
    }

    pos = next_pos + 1;
  }

  return true;
}

std::string PathHelper::BuildFilePath(const std::string& directory,
                                      const std::string& filename) {
  if (directory.empty()) {
    return filename;
  }

  char sep = GetPathSeparator();
  if (directory.back() == '/' || directory.back() == '\\') {
    return directory + filename;
  }
  return directory + sep + filename;
}

bool PathHelper::EnsureParentDirectory(const std::string& file_path) {
  std::filesystem::path path(file_path);
  auto parent = path.parent_path();
  if (parent.empty()) {
    return true;
  }

  std::error_code ec;
  if (std::filesystem::exists(parent, ec)) {
    return true;
  }

  return std::filesystem::create_directories(parent, ec);
}

char PathHelper::GetPathSeparator() {
#ifdef QUATON_PLATFORM_WINDOWS
  return '\\';
#else
  return '/';
#endif
}

std::string PathHelper::NormalizePath(const std::string& path) {
  std::string result = path;
#ifdef QUATON_PLATFORM_WINDOWS
  for (char& c : result) {
    if (c == '/') c = '\\';
  }
#else
  for (char& c : result) {
    if (c == '\\') c = '/';
  }
#endif
  return result;
}

QUATON_NAMESPACE_END
