#include "quaton/configuration/path_config.h"

#include "quaton/utils/path_helper.h"

namespace Quaton {

PathConfig PathConfig::create_default() {
  PathConfig config;
  // PathHelper owns the data root so an override applies to the database,
  // manifests, temporary files and logs alike.
  config.initialize_from_base(PathHelper::GetDataDir());
  return config;
}

}  // namespace Quaton
