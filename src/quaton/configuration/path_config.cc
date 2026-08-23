#include "quaton/configuration/path_config.h"

#include <cstdlib>

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#endif

namespace Quaton {

PathConfig PathConfig::create_default() {
  PathConfig config;

  std::filesystem::path base_dir;

#ifdef _WIN32
  // Windows: Use %APPDATA%\PremiX\Quaton (consistent with PathHelper)
  const char* app_data = std::getenv("APPDATA");
  if (app_data) {
    base_dir = std::filesystem::path(app_data) / "PremiX" / "Quaton";
  } else {
    base_dir = std::filesystem::path("PremiX") / "Quaton";
  }
#else
  // Linux/Mac: Use ~/.config/PremiX/Quaton (consistent with PathHelper)
  const char* home = std::getenv("HOME");
  if (home) {
    base_dir = std::filesystem::path(home) / ".config" / "PremiX" / "Quaton";
  } else {
    base_dir = std::filesystem::path("PremiX") / "Quaton";
  }
#endif

  config.initialize_from_base(base_dir);
  return config;
}

}  // namespace Quaton
