#include "quaton/url.h"

#include <curl/curl.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>

#include "quaton/logger.h"

namespace Quaton {

// Compile-time constants
constexpr size_t kMaxUrlLength = 2048;
constexpr int kCurlTimeoutSeconds = 30;

// CURL write callback function
static size_t WriteCallback(void* contents,
                            size_t size,
                            size_t nmemb,
                            std::string* user_data) {
  size_t total_size = size * nmemb;
  user_data->append(static_cast<char*>(contents), total_size);
  return total_size;
}

static std::string FormatVersionNumber(std::string_view version) {
  if (version.empty()) {
    return std::string(version);
  }

  size_t dot_count = std::count(version.begin(), version.end(), '.');

  if (dot_count >= 2) {
    return std::string(version);
  }

  // If in x.x format, add .0 -> x.x.0
  if (dot_count == 1) {
    return std::string(version) + ".0";
  }

  return std::string(version);
}

Url::Url(BranchType branch) : branch_(branch) {
  LOG_DEBUG("Url constructor - self-hosted mode, branch: %d",
            static_cast<int>(branch));
}

Url::Url(Url&& other) noexcept
    : api_base_(std::move(other.api_base_)),
      branch_(other.branch_),
      package_id_(std::move(other.package_id_)),
      password_(std::move(other.password_)),
      branch_backup_(std::move(other.branch_backup_)) {
  other.branch_ = BranchType::kMain;
}

Url& Url::operator=(Url&& other) noexcept {
  if (this != &other) {
    api_base_ = std::move(other.api_base_);
    branch_ = other.branch_;
    package_id_ = std::move(other.package_id_);
    password_ = std::move(other.password_);
    branch_backup_ = std::move(other.branch_backup_);

    other.branch_ = BranchType::kMain;
  }
  return *this;
}

std::future<int> Url::GetBuildData() {
  return std::async(std::launch::async, [this]() -> int {
    try {
      // getBranches returns the flat branch index (no game wrapper).
      std::string url = api_base_ + "/getBranches";

      std::string json_str = fetch_url(url).get();

      LOG_DEBUG("API URL: %s", url.c_str());
      LOG_DEBUG("API Response: %s", json_str.substr(0, 1000).c_str());

      if (json_str.empty()) {
        LOG_ERROR("Empty API response received");
        return -1;
      }

      nlohmann::json json = nlohmann::json::parse(json_str);

      if (json.is_null()) {
        LOG_ERROR("Parsed JSON is null");
        return -1;
      }

      BranchesRoot root;
      root.retcode_ = json["retcode"];
      root.message_ = json["message"];

      // Simplified launcher-adjacent protocol: data.branches is a flat array
      // with no game wrapper and no pre_download.
      if (!json["data"]["branches"].empty()) {
        auto& branch_json = json["data"]["branches"][0];
        root.data_.game_branches_.emplace_back();
        auto& game_branch = root.data_.game_branches_.back();

        game_branch.main_.branch_ =
            branch_json.value("branch", std::string("main"));
        game_branch.main_.password_ =
            branch_json.value("password", std::string());
        game_branch.main_.tag_ = branch_json.value("tag", std::string());

        if (branch_json.contains("diff_tags")) {
          for (const auto& tag : branch_json["diff_tags"]) {
            game_branch.main_.diff_tags_.push_back(tag);
          }
        }

        if (branch_json.contains("categories")) {
          for (const auto& cat : branch_json["categories"]) {
            BranchesCategory category;
            category.category_id_ = cat.value("category_id", std::string());
            category.matching_field_ =
                cat.value("matching_field", std::string());
            game_branch.main_.categories_.push_back(category);
          }
        }
      }

      std::vector<std::string> data = parse_build_data(root, branch_);
      if (data[0] != "OK") {
        LOG_ERROR("Error: %s", std::string(data[1]).c_str());
        return -1;
      }

      package_id_ = data[1];
      password_ = data[2];
      branch_backup_ = std::move(root);

      return 0;
    } catch (const std::exception& e) {
      LOG_ERROR("Exception in GetBuildData: %s", e.what());
      return -1;
    }
  });
}

std::string Url::GetBuildUrl(const std::string& version, bool is_update) {
  LOG_DEBUG("GetBuildUrl called with version=\'%s\', is_update=%d",
            version.c_str(),
            is_update);

  // Launcher-adjacent protocol: getBuild is addressed by tag only.
  std::string url = api_base_ + "/getBuild?tag=" + version;

  if (url.length() > kMaxUrlLength) {
    LOG_ERROR("Generated URL exceeds maximum length: %zu", url.length());
    throw std::length_error("URL length exceeds maximum allowed");
  }

  LOG_DEBUG("Final URL: %s", url.c_str());
  return url;
}

std::vector<std::string> Url::parse_build_data(const BranchesRoot& root,
                                               BranchType search_branch) {
  if (root.retcode_ != 0 && root.message_ != "OK") {
    return {"ERROR", root.message_};
  }

  auto branch = get_branch(root, search_branch);
  if (!branch) {
    return {
        "ERROR",
        "Branch " +
            std::string(search_branch == BranchType::kMain ? "main"
                                                           : "pre_download") +
            " not found"};
  }

  // The launcher-adjacent protocol has no game object; package_id no longer
  // exists either, so only the password is carried over.
  return {"OK", "", branch->password_};
}

std::unique_ptr<BranchesMain> Url::get_branch(const BranchesRoot& root,
                                              BranchType search_branch) {
  if (root.data_.game_branches_.empty()) {
    return nullptr;
  }

  const auto& game_branch = root.data_.game_branches_[0];

  if (search_branch == BranchType::kMain) {
    return std::make_unique<BranchesMain>(game_branch.main_);
  } else if (search_branch == BranchType::kPreDownload) {
    return std::make_unique<BranchesMain>(game_branch.pre_download_);
  }

  return nullptr;
}

std::future<std::string> Url::fetch_url(const std::string& url) {
  return std::async(std::launch::async, [url]() -> std::string {
    // Initialize CURL globally
    static std::once_flag curl_init_flag;
    std::call_once(curl_init_flag,
                   []() { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL* curl = curl_easy_init();
    if (!curl) {
      throw std::runtime_error("Failed to initialize CURL");
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    // Verify the peer and host: no measurable cost (one handshake per
    // connection) and protects against MITM on HTTPS endpoints. HTTP URLs
    // are unaffected by these settings.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, kCurlTimeoutSeconds);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
      throw std::runtime_error("CURL request failed: " +
                               std::string(curl_easy_strerror(res)));
    }

    return response;
  });
}

std::string Url::BuildChunkUrl(const Quaton::ChunkInfo& chunks_info,
                               const std::string& chunk_name) {
  // The url_prefix from the server ends with '/', so trim it before joining
  // to avoid a double slash that S3-compatible buckets reject with HTTP 400.
  std::string base = chunks_info.get_base_url();
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  return base + "/" + chunk_name;
}

void Url::SetApiBase(const std::string& base) {
  std::string trimmed = base;
  while (!trimmed.empty() && trimmed.back() == '/') {
    trimmed.pop_back();
  }
  if (trimmed.empty()) {
    return;
  }
  api_base_ = trimmed;
}

std::string Url::GetPatchBuildUrl(const std::string& update_from,
                                  const std::string& update_to) {
  LOG_DEBUG("GetPatchBuildUrl called with update_from='%s', update_to='%s'",
            update_from.c_str(),
            update_to.c_str());

  std::string url = api_base_ + "/getPatchBuild?tag=" + update_to;

  LOG_DEBUG("Final getPatchBuild URL: %s", url.c_str());
  return url;
}

bool Url::CanUpdateFrom(const std::string& source_version,
                        const std::string& target_version) {
  if (branch_backup_.data_.game_branches_.empty()) {
    LOG_ERROR("No game branch data available");
    return false;
  }

  const auto& game_branch = branch_backup_.data_.game_branches_[0];
  const BranchesMain* branch = nullptr;

  // Select corresponding branch
  if (branch_ == BranchType::kMain) {
    branch = &game_branch.main_;
  } else {
    branch = &game_branch.pre_download_;
  }

  if (!branch) {
    LOG_ERROR("Branch not found");
    return false;
  }

  // Check if source version is in diff_tags list
  for (const auto& diff_tag : branch->diff_tags_) {
    if (diff_tag == source_version) {
      LOG_INFO("Source version %s can be updated (found in diff_tags)",
               source_version.c_str());
      return true;
    }
  }

  LOG_INFO("Source version %s cannot use LDIFF update (not in diff_tags)",
           source_version.c_str());
  return false;
}

}  // namespace Quaton