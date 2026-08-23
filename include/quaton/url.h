#pragma once

#include <future>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "quaton/data_chunk.h"
#include "quaton/quaton_global.h"  // For QUATON_API macro

namespace Quaton {

/**
 * @enum BranchType
 * @brief Branch type enumeration
 */
enum class BranchType {
  kMain,        ///< Main branch
  kPreDownload  ///< Pre-download branch
};

// Forward declarations
struct BranchesCategory;
struct BranchesMain;
struct BranchesGame;
struct BranchesGameBranch;
struct BranchesData;
struct BranchesRoot;

/**
 * @struct BranchesCategory
 * @brief Branch category structure
 */
struct BranchesCategory {
  std::string category_id_;     ///< Category ID
  std::string matching_field_;  ///< Matching field
};

/**
 * @struct BranchesMain
 * @brief Main branch info structure
 */
struct BranchesMain {
  std::string package_id_;                    ///< Package ID
  std::string branch_;                        ///< Branch name
  std::string password_;                      ///< Password
  std::string tag_;                           ///< Tag
  std::vector<std::string> diff_tags_;        ///< Diff tags list
  std::vector<BranchesCategory> categories_;  ///< Categories list
};

/**
 * @struct BranchesGame
 * @brief Game info structure
 */
struct BranchesGame {
  std::string id_;  ///< Game ID
};

/**
 * @struct BranchesGameBranch
 * @brief Game branch structure
 */
struct BranchesGameBranch {
  BranchesGame game_;          ///< Game info
  BranchesMain main_;          ///< Main branch
  BranchesMain pre_download_;  ///< Pre-download branch
};

/**
 * @struct BranchesData
 * @brief Branch data structure
 */
struct BranchesData {
  std::vector<BranchesGameBranch> game_branches_;  ///< Game branches list
};

/**
 * @struct BranchesRoot
 * @brief Branch root structure
 */
struct BranchesRoot {
  int retcode_;          ///< Return code
  std::string message_;  ///< Message
  BranchesData data_;    ///< Data
};

/**
 * @class Url
 * @brief Quaton URL building and management class
 *
 * Responsible for building Quaton download related URLs and managing game
 * branch information
 */
class QUATON_API Url {
 public:
  /**
   * @brief Constructor for self-hosted mode
   * @param branch Branch type, default kMain
   */
  explicit Url(BranchType branch = BranchType::kMain);

  /**
   * @brief Move constructor
   * @param other Url object to move
   */
  Url(Url&& other) noexcept;

  /**
   * @brief Move assignment operator
   * @param other Url object to move
   * @return Reference to self
   */
  Url& operator=(Url&& other) noexcept;

  // Disable copy construction and assignment
  Url(const Url&) = delete;
  Url& operator=(const Url&) = delete;

  /**
   * @brief Override the API base URL (used by getBranches/getBuild/
   * getPatchBuild). When set, it takes precedence over the default.
   * @param base Server base URL, e.g. "http://example.com:10001"
   */
  void SetApiBase(const std::string& base);

  /**
   * @brief Get build data
   * @return Future that returns 0 on success, 1 on failure
   */
  std::future<int> GetBuildData();

  /**
   * @brief Get build URL
   * @param version Version string
   * @param is_update Whether it's an update
   * @return Build URL string
   */
  std::string GetBuildUrl(const std::string& version, bool is_update = false);

  /**
   * @brief Build chunk URL
   * @param chunks_info Chunk info
   * @param chunk_name Chunk name
   * @return Chunk URL string
   */
  static std::string BuildChunkUrl(const Quaton::ChunkInfo& chunks_info,
                                   const std::string& chunk_name);

  /**
   * @brief Get Package ID
   * @return Package ID string
   */
  std::string GetPackageId() const noexcept { return package_id_; }

  /**
   * @brief Get branch backup data (containing complete API response)
   * @return BranchesRoot reference
   */
  const BranchesRoot& GetBranchBackup() const noexcept {
    return branch_backup_;
  }

  /**
   * @brief Get getPatchBuild API URL
   * @param update_from Update starting version
   * @param update_to Update target version
   * @return getPatchBuild API URL
   */
  std::string GetPatchBuildUrl(const std::string& update_from,
                               const std::string& update_to);

  /**
   * @brief Check if specified source version can update to target version
   * @param source_version Source version number
   * @param target_version Target version number (optional, defaults to current
   * branch tag)
   * @return Returns true if source version is in diff_tags list
   */
  bool CanUpdateFrom(const std::string& source_version,
                     const std::string& target_version = "");

 private:
  std::string api_base_;        ///< API base URL
  BranchType branch_;           ///< Branch type
  std::string package_id_;      ///< Package ID
  std::string password_;        ///< Password
  BranchesRoot branch_backup_;  ///< Branch backup data

  /**
   * @brief Parse build data
   * @param root Branch root data
   * @param search_branch Search branch
   * @return Parse result array
   */
  std::vector<std::string> parse_build_data(const BranchesRoot& root,
                                            BranchType search_branch);

  /**
   * @brief Get branch info
   * @param root Branch root data
   * @param search_branch Search branch
   * @return Branch info pointer, returns nullptr if not exist
   */
  static std::unique_ptr<BranchesMain> get_branch(const BranchesRoot& root,
                                                  BranchType search_branch);

  /**
   * @brief Fetch content from URL
   * @param url URL address
   * @return Future of fetched content string
   */
  static std::future<std::string> fetch_url(const std::string& url);
};

}  // namespace Quaton
