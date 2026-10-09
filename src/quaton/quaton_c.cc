// C ABI for hosts that consume Quaton through LoadLibrary instead of the
// import library. Nothing crosses the boundary except plain data: results are
// copied into caller-owned buffers and every structured value is JSON, so a
// host built against a different CRT never frees Quaton memory.

#include "quaton/quaton_c.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "quaton/chunk/chunk_download_service.h"
#include "quaton/http_client.h"
#include "quaton/logger.h"
#include "quaton/manifest/json_parser.h"
#include "quaton/manifest/manifest_processor_patch.h"
#include "quaton/patch/patch_download_service.h"
#include "quaton/progress_callback.h"
#include "quaton/quaton.h"
#include "quaton/url.h"
#include "quaton/utils/path_helper.h"

namespace {

using nlohmann::json;

// ============================================================================
// Error reporting and buffer plumbing
// ============================================================================

std::mutex g_error_mutex;
std::string g_last_error;

void SetLastError(std::string message) {
  std::lock_guard<std::mutex> lock(g_error_mutex);
  g_last_error = std::move(message);
}

int32_t Fail(std::string message) {
  SetLastError(std::move(message));
  return -1;
}

// Every export clears the error on entry so that a successful call always
// leaves the last error empty, as documented in quaton_c.h.
void ClearLastError() {
  std::lock_guard<std::mutex> lock(g_error_mutex);
  g_last_error.clear();
}

// File system paths are not guaranteed to be valid UTF-8 on Windows, so invalid
// bytes are replaced instead of letting dump() throw across the C boundary.
std::string DumpJson(const json& value) {
  return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

int32_t CopyToBuffer(const std::string& text, char* buffer, int32_t size) {
  if (text.size() > static_cast<size_t>(INT32_MAX)) {
    return Fail("result is too large for the C ABI buffer");
  }
  const auto needed = static_cast<int32_t>(text.size());
  if (buffer != nullptr && size > needed) {
    std::memcpy(buffer, text.c_str(), static_cast<size_t>(needed) + 1);
  }
  return needed;
}

// ============================================================================
// Request helpers
// ============================================================================

std::string GetString(const json& object,
                      const char* key,
                      const std::string& fallback = std::string()) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_string()) {
    return fallback;
  }
  return it->get<std::string>();
}

// Numbers are read through double, so an out-of-range value falls back instead
// of throwing json::out_of_range through the C ABI.
int GetInt(const json& object, const char* key, int fallback) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_number()) {
    return fallback;
  }
  const double value = it->get<double>();
  if (!(value >= -2147483648.0 && value <= 2147483647.0)) {
    return fallback;
  }
  return static_cast<int>(value);
}

bool GetBool(const json& object, const char* key, bool fallback) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_boolean()) {
    return fallback;
  }
  return it->get<bool>();
}

bool ParseRequest(const char* request_json, json* request, std::string* error) {
  if (request_json == nullptr || *request_json == '\0') {
    *error = "request JSON is empty";
    return false;
  }
  try {
    json parsed = json::parse(request_json);
    if (!parsed.is_object()) {
      *error = "request JSON must be an object";
      return false;
    }
    *request = std::move(parsed);
    return true;
  } catch (const std::exception& e) {
    *error = std::string("request JSON is invalid: ") + e.what();
    return false;
  }
}

// ============================================================================
// HTTP helpers
// ============================================================================

std::shared_ptr<Quaton::HttpClient> MakeHttpClient() {
  return std::make_shared<Quaton::HttpClient>(64);
}

// The API occasionally drops a connection mid-response, so every index request
// gets a couple of cheap retries before the caller sees a failure.
constexpr int kRequestAttempts = 3;

bool FetchText(const std::shared_ptr<Quaton::HttpClient>& http,
               const std::string& url,
               bool use_post,
               std::string* out,
               std::string* error) {
  for (int attempt = 1; attempt <= kRequestAttempts; ++attempt) {
    try {
      const std::vector<uint8_t> body =
          use_post ? http->post_async(url, std::string()).get()
                   : http->get_async(url).get();
      if (body.empty()) {
        *error = "empty response from " + url;
      } else {
        out->assign(body.begin(), body.end());
        return true;
      }
    } catch (const std::exception& e) {
      *error = std::string("request failed: ") + url + ": " + e.what();
    }
    if (attempt != kRequestAttempts) {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
  }
  return false;
}

bool FetchJson(const std::shared_ptr<Quaton::HttpClient>& http,
               const std::string& url,
               bool use_post,
               json* out,
               std::string* error) {
  std::string text;
  if (!FetchText(http, url, use_post, &text, error)) {
    return false;
  }
  try {
    *out = json::parse(text);
    return true;
  } catch (const std::exception& e) {
    *error = std::string("invalid JSON from ") + url + ": " + e.what();
    return false;
  }
}

// Manifest files live directly under a prefix that already ends in a slash.
std::string JoinUrl(const std::string& prefix, const std::string& name) {
  if (prefix.empty()) {
    return name;
  }
  std::string base = prefix;
  if (base.back() == '/') {
    base.pop_back();
  }
  return name.empty() ? base : base + "/" + name;
}

// ============================================================================
// Index model
// ============================================================================

struct CategoryInfo {
  std::string category_id;
  std::string manifest_id;
  std::string manifest_checksum;
  int64_t manifest_compressed_size = 0;
  int64_t manifest_uncompressed_size = 0;
  std::string manifest_url;
  std::string chunk_url_prefix;
  int64_t file_count = 0;
  int64_t chunk_count = 0;
  int64_t compressed_size = 0;
  int64_t uncompressed_size = 0;

  // Filled from getPatchBuild when a previous tag was supplied.
  bool patch_available = false;
  std::string patch_manifest_id;
  std::string patch_manifest_checksum;
  std::string patch_manifest_url;
  std::string patch_url_prefix;
  int64_t patch_compressed_size = 0;
  int64_t patch_uncompressed_size = 0;
};

struct IndexInfo {
  std::string tag;
  std::vector<std::string> diff_tags;
  std::vector<CategoryInfo> categories;

  const CategoryInfo* Find(const std::string& category_id) const {
    for (const auto& category : categories) {
      if (category.category_id == category_id) {
        return &category;
      }
    }
    return nullptr;
  }
};

// Reads getBranches, then getBuild for the resolved tag and optionally
// getPatchBuild for the previous tag.
bool QueryIndex(const std::string& api_base_url,
                const std::string& branch,
                const std::string& requested_tag,
                const std::string& previous_tag,
                IndexInfo* out,
                std::string* error) {
  if (api_base_url.empty()) {
    *error = "api_base_url is required";
    return false;
  }

  const bool pre_download =
      (branch == "predownload" || branch == "pre-download");
  Quaton::Url url(pre_download ? Quaton::BranchType::kPreDownload
                               : Quaton::BranchType::kMain);
  url.SetApiBase(api_base_url);

  bool branches_ok = false;
  for (int attempt = 1; attempt <= kRequestAttempts && !branches_ok;
       ++attempt) {
    branches_ok = (url.GetBuildData().get() == 0);
    if (!branches_ok && attempt != kRequestAttempts) {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
  }
  if (!branches_ok) {
    *error = "getBranches failed for " + api_base_url;
    return false;
  }

  std::string head_tag;
  const Quaton::BranchesRoot& root = url.GetBranchBackup();
  for (const auto& game_branch : root.data_.game_branches_) {
    const Quaton::BranchesMain& entry =
        pre_download ? game_branch.pre_download_ : game_branch.main_;
    if (!entry.tag_.empty()) {
      head_tag = entry.tag_;
      out->diff_tags = entry.diff_tags_;
      break;
    }
  }

  out->tag = requested_tag.empty() ? head_tag : requested_tag;
  if (out->tag.empty()) {
    *error = "the branch has no published tag";
    return false;
  }

  auto http = MakeHttpClient();

  json build_json;
  if (!FetchJson(http, url.GetBuildUrl(out->tag), false, &build_json, error)) {
    return false;
  }
  Quaton::BuildApiResponse build;
  try {
    Quaton::parse_from_json(build_json, &build);
  } catch (const std::exception& e) {
    *error = std::string("cannot parse getBuild response: ") + e.what();
    return false;
  }
  if (!build.is_successful()) {
    *error = "getBuild rejected: " + build.status_message();
    return false;
  }

  for (const auto& category : build.response_payload().manifest_categories()) {
    CategoryInfo info;
    info.category_id = category.category_identifier();
    info.manifest_id = category.manifest_metadata().identifier();
    info.manifest_checksum = category.manifest_metadata().checksum();
    info.manifest_compressed_size =
        category.manifest_metadata().compressed_size_bytes();
    info.manifest_uncompressed_size =
        category.manifest_metadata().uncompressed_size_bytes();
    info.manifest_url =
        JoinUrl(category.manifest_download_config().construct_full_url(),
                info.manifest_id);
    info.chunk_url_prefix =
        category.chunk_download_config().construct_full_url();
    info.file_count = category.chunk_statistics().file_count();
    info.chunk_count = category.chunk_statistics().chunk_count();
    info.compressed_size = category.chunk_statistics().compressed_size_bytes();
    info.uncompressed_size =
        category.chunk_statistics().uncompressed_size_bytes();
    out->categories.push_back(std::move(info));
  }

  if (previous_tag.empty()) {
    return true;
  }

  // Patch availability is only meaningful for a known source version.
  bool can_update = false;
  for (const auto& diff_tag : out->diff_tags) {
    if (diff_tag == previous_tag) {
      can_update = true;
      break;
    }
  }
  if (!can_update) {
    return true;
  }

  json patch_json;
  if (!FetchJson(http,
                 url.GetPatchBuildUrl(previous_tag, out->tag),
                 true,
                 &patch_json,
                 error)) {
    // A missing patch set only means the caller has to download in full.
    error->clear();
    return true;
  }

  Quaton::PatchBuildApiResponse patch_build;
  try {
    Quaton::parse_from_json(patch_json, &patch_build);
  } catch (const std::exception&) {
    return true;
  }
  if (!patch_build.is_successful()) {
    return true;
  }

  for (const auto& patch_category :
       patch_build.response_payload().patch_manifest_categories()) {
    const std::string& category_id = patch_category.category_identifier();
    for (auto& category : out->categories) {
      if (category.category_id != category_id) {
        continue;
      }
      if (!patch_category.supports_version(previous_tag)) {
        break;
      }
      const auto* stats =
          patch_category.get_statistics_for_version(previous_tag);
      category.patch_available = stats != nullptr;
      category.patch_manifest_id =
          patch_category.manifest_metadata().identifier();
      category.patch_manifest_checksum =
          patch_category.manifest_metadata().checksum();
      category.patch_manifest_url = JoinUrl(
          patch_category.manifest_download_config().construct_full_url(),
          category.patch_manifest_id);
      category.patch_url_prefix =
          patch_category.differential_download_config().construct_full_url();
      if (stats != nullptr) {
        category.patch_compressed_size = stats->compressed_size_bytes();
        category.patch_uncompressed_size = stats->uncompressed_size_bytes();
      }
      break;
    }
  }
  return true;
}

json IndexToJson(const IndexInfo& index) {
  json categories = json::array();
  for (const auto& category : index.categories) {
    json patch = {{"available", category.patch_available},
                  {"manifest_id", category.patch_manifest_id},
                  {"manifest_checksum", category.patch_manifest_checksum},
                  {"manifest_url", category.patch_manifest_url},
                  {"url_prefix", category.patch_url_prefix},
                  {"compressed_size", category.patch_compressed_size},
                  {"uncompressed_size", category.patch_uncompressed_size}};
    categories.push_back(
        {{"category_id", category.category_id},
         {"manifest_id", category.manifest_id},
         {"manifest_checksum", category.manifest_checksum},
         {"manifest_url", category.manifest_url},
         {"compressed_size", category.manifest_compressed_size},
         {"uncompressed_size", category.manifest_uncompressed_size},
         {"chunk_url_prefix", category.chunk_url_prefix},
         {"file_count", category.file_count},
         {"chunk_count", category.chunk_count},
         {"download_size", category.compressed_size},
         {"install_size", category.uncompressed_size},
         {"patch", std::move(patch)}});
  }
  return {{"success", true},
          {"tag", index.tag},
          {"diff_tags", index.diff_tags},
          {"categories", std::move(categories)}};
}

// ============================================================================
// Sessions
// ============================================================================

enum SessionState { kRunning = 0, kSucceeded = 1, kFailed = 2, kCancelled = 3 };

const char* StateName(int state) {
  switch (state) {
    case kSucceeded:
      return "succeeded";
    case kFailed:
      return "failed";
    case kCancelled:
      return "cancelled";
    default:
      return "running";
  }
}

struct Session {
  int64_t id = 0;
  std::atomic<int> state{kRunning};
  std::atomic<int> result_code{0};
  std::atomic<bool> cancel_requested{false};

  std::mutex mutex;  // guards message, progress and service
  std::string message;
  json progress = json::object();
  std::shared_ptr<Quaton::DownloadService> service;

  std::mutex join_mutex;  // guards worker/joined
  std::thread worker;
  bool joined = false;

  quaton_progress_callback_c callback = nullptr;
  void* user_data = nullptr;

  void Finish(int code) {
    result_code.store(code);
    if (cancel_requested.load() && code != 0) {
      state.store(kCancelled);
    } else {
      state.store(code == 0 ? kSucceeded : kFailed);
    }
  }

  void FinishWithError(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex);
    message = error;
    result_code.store(-1);
    state.store(cancel_requested.load() ? kCancelled : kFailed);
  }
};

std::mutex g_sessions_mutex;
std::map<int64_t, std::shared_ptr<Session>> g_sessions;
std::atomic<int64_t> g_next_session_id{1};

void RegisterSession(const std::shared_ptr<Session>& session) {
  std::lock_guard<std::mutex> lock(g_sessions_mutex);
  g_sessions[session->id] = session;
}

std::shared_ptr<Session> FindSession(int64_t id) {
  std::lock_guard<std::mutex> lock(g_sessions_mutex);
  const auto it = g_sessions.find(id);
  return it == g_sessions.end() ? nullptr : it->second;
}

std::shared_ptr<Session> TakeSession(int64_t id) {
  std::lock_guard<std::mutex> lock(g_sessions_mutex);
  const auto it = g_sessions.find(id);
  if (it == g_sessions.end()) {
    return nullptr;
  }
  auto session = it->second;
  g_sessions.erase(it);
  return session;
}

// ============================================================================
// Progress reporting
// ============================================================================

const char* OperationModeName(Quaton::OperationMode mode) {
  switch (mode) {
    case Quaton::OperationMode::kPatchUpdate:
      return "patch_update";
    case Quaton::OperationMode::kPatchPredownload:
      return "patch_predownload";
    case Quaton::OperationMode::kPatchLocalInstall:
      return "patch_local_install";
    default:
      return "chunk_download";
  }
}

json ProgressToJson(const Quaton::ProgressInfo& info) {
  const Quaton::FileProcessStatus& status = info.current_status;
  return {{"mode", OperationModeName(info.operation_mode)},
          {"percent", info.overall_percentage},
          {"current_file", info.current_file},
          {"total_files", info.total_files},
          {"completed_files", info.completed_files},
          {"remaining_files", info.remaining_files},
          {"failed_files", info.failed_files},
          {"speed_mbps", info.download_speed},
          {"eta_seconds", info.estimated_seconds},
          {"stages",
           {{"download_initiated", status.download_initiated},
            {"download_completed", status.download_completed},
            {"decompressed", status.decompressed},
            {"verified", status.verified},
            {"applied", status.applied}}}};
}

// Called from the library's download threads.
void ReportProgress(const std::shared_ptr<Session>& session,
                    const Quaton::ProgressInfo& info) {
  json payload = ProgressToJson(info);
  quaton_progress_callback_c callback = nullptr;
  void* user_data = nullptr;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    session->progress = payload;
    callback = session->callback;
    user_data = session->user_data;
  }
  if (callback != nullptr) {
    const std::string text = DumpJson(payload);
    // A throwing host callback must not unwind into the library's threads.
    try {
      callback(text.c_str(), user_data);
    } catch (...) {
    }
  }
}

// Holds the running service for the duration of a download. The progress
// callback captures the session, so the service would keep the session alive in
// return; clearing the callback and the session's reference is what breaks that
// cycle, and it has to happen on every exit path, including early failures.
class ServiceOwnership {
 public:
  ServiceOwnership(const std::shared_ptr<Session>& session,
                   std::shared_ptr<Quaton::DownloadService> service)
      : session_(session), service_(std::move(service)) {
    std::lock_guard<std::mutex> lock(session_->mutex);
    session_->service = service_;
  }

  ~ServiceOwnership() {
    service_->SetProgressCallback(nullptr);
    std::lock_guard<std::mutex> lock(session_->mutex);
    session_->service.reset();
  }

  ServiceOwnership(const ServiceOwnership&) = delete;
  ServiceOwnership& operator=(const ServiceOwnership&) = delete;

 private:
  std::shared_ptr<Session> session_;
  std::shared_ptr<Quaton::DownloadService> service_;
};

// ============================================================================
// Download workers
// ============================================================================

int RunChunkDownload(const std::shared_ptr<Session>& session,
                     const json& request,
                     std::string* error) {
  const std::string manifest_url = GetString(request, "current_manifest_url");
  const std::string chunk_url = GetString(request, "chunk_base_url");
  std::string install_dir = GetString(request, "install_dir");
  if (install_dir.empty()) {
    install_dir = GetString(request, "output_dir");
  }
  if (manifest_url.empty() || chunk_url.empty() || install_dir.empty()) {
    *error =
        "chunk mode requires current_manifest_url, chunk_base_url and "
        "install_dir";
    return -1;
  }

  Quaton::ChunkServiceConfig config;
  config.thread_count = GetInt(request, "threads", 0);
  config.max_http_handles = GetInt(request, "max_http_handles", 128);
  config.retry_count = GetInt(request, "retry_count", 5);
  config.verify_downloads = GetBool(request, "verify_downloads", true);
  config.silent = GetBool(request, "silent", false);
  config.install_path = install_dir;
  config.temp_path = GetString(request, "temp_dir");
  config.manifest_output_dir = GetString(request, "manifest_dir");

  auto service = std::make_shared<Quaton::ChunkDownloadService>();
  ServiceOwnership ownership(session, service);
  service->SetProgressCallback([session](const Quaton::ProgressInfo& info) {
    ReportProgress(session, info);
  });

  if (!service->Initialize(config)) {
    *error = "chunk download service failed to initialize";
    return -1;
  }

  const int code = service
                       ->Download(GetString(request, "previous_manifest_url"),
                                  manifest_url,
                                  chunk_url,
                                  install_dir,
                                  GetString(request, "filter"))
                       .get();
  if (code != 0) {
    *error = "chunk download returned " + std::to_string(code);
  }
  return code;
}

int RunPatchDownload(const std::shared_ptr<Session>& session,
                     const json& request,
                     std::string* error) {
  std::string install_dir = GetString(request, "install_dir");
  if (install_dir.empty()) {
    install_dir = GetString(request, "output_dir");
  }
  const std::string package_name =
      GetString(request, "category_id", GetString(request, "package_name"));
  std::string manifest_url = GetString(request, "patch_manifest_url");
  std::string manifest_id = GetString(request, "patch_manifest_id");
  std::string manifest_md5 = GetString(request, "patch_manifest_md5");
  std::string patch_url_prefix = GetString(request, "patch_url_prefix");
  const std::string source_version =
      GetString(request, "source_version", GetString(request, "previous_tag"));
  bool manifest_compressed =
      GetBool(request, "patch_manifest_compressed", true);

  // Anything the caller left out comes from the index, so a host can trigger a
  // patch update with just the api base, the category and the install dir.
  if (manifest_url.empty() || patch_url_prefix.empty()) {
    const std::string api_base_url = GetString(request, "api_base_url");
    IndexInfo index;
    std::string index_error;
    if (!QueryIndex(api_base_url,
                    GetString(request, "branch", "main"),
                    GetString(request, "tag"),
                    source_version,
                    &index,
                    &index_error)) {
      *error = index_error;
      return -1;
    }
    const CategoryInfo* category = index.Find(package_name);
    if (category == nullptr) {
      *error = "category not present in the index: " + package_name;
      return -1;
    }
    if (!category->patch_available) {
      *error = "no patch set for source version: " + source_version;
      return -1;
    }
    if (manifest_url.empty()) {
      manifest_url = category->patch_manifest_url;
      manifest_id = category->patch_manifest_id;
      manifest_md5 = category->patch_manifest_checksum;
      manifest_compressed = true;
    }
    if (patch_url_prefix.empty()) {
      patch_url_prefix = category->patch_url_prefix;
    }
  }

  if (install_dir.empty() || manifest_url.empty() || patch_url_prefix.empty()) {
    *error =
        "patch mode requires install_dir, patch_manifest_url and "
        "patch_url_prefix";
    return -1;
  }

  const bool predownload = GetBool(request, "predownload_only", false);

  Quaton::PatchServiceConfig config;
  config.thread_count = GetInt(request, "threads", 0);
  config.max_http_handles = GetInt(request, "max_http_handles", 128);
  config.verify_downloads = GetBool(request, "verify_downloads", true);
  config.silent = GetBool(request, "silent", false);
  config.mode = predownload ? Quaton::PatchDownloadMode::kPreDownload
                            : Quaton::PatchDownloadMode::kDownloadAndApply;
  config.download_threads = GetInt(request, "download_threads", 0);
  config.apply_threads = GetInt(request, "apply_threads", 0);
  config.backup_path = GetString(request, "backup_dir");
  config.create_backup = GetBool(request, "create_backup", true);
  config.rollback_on_failure = GetBool(request, "rollback_on_failure", true);
  config.temp_patch_dir = GetString(request, "temp_dir");
  config.manager_config.target_path = install_dir;
  config.manager_config.source_path = install_dir;
  config.manager_config.temp_path = config.temp_patch_dir;
  config.manager_config.patch_base_url = patch_url_prefix;
  config.manager_config.source_version = source_version;
  config.manager_config.target_version = GetString(request, "tag");

  auto service = std::make_shared<Quaton::PatchDownloadService>();
  ServiceOwnership ownership(session, service);
  service->SetProgressCallback([session](const Quaton::ProgressInfo& info) {
    ReportProgress(session, info);
  });

  if (!service->Initialize(config)) {
    *error = "patch download service failed to initialize";
    return -1;
  }

  try {
    auto manifest = Quaton::PatchManifestProcessor::download_and_parse_manifest(
        MakeHttpClient(),
        manifest_url,
        manifest_compressed,
        manifest_md5,
        source_version,
        manifest_id,
        GetString(request, "manifest_dir"));
    if (manifest == nullptr) {
      *error = "patch manifest could not be read: " + manifest_url;
      return -1;
    }
    if (service->PrepareDownloadTasks(manifest,
                                      package_name,
                                      patch_url_prefix,
                                      GetString(request, "patch_url_suffix")) <
        0) {
      *error = "patch manifest could not be prepared";
      return -1;
    }
    const int code = service->Execute().get();
    if (code != 0) {
      *error = "patch download returned " + std::to_string(code);
    }
    return code;
  } catch (const std::exception& e) {
    *error = std::string("patch update failed: ") + e.what();
    return -1;
  }
}

// Resolves "auto" to either a full chunk download or a patch update.
bool BuildAutoRequest(const json& request, json* resolved, std::string* error) {
  const std::string api_base_url = GetString(request, "api_base_url");
  const std::string category_id = GetString(request, "category_id");
  const std::string previous_tag = GetString(request, "previous_tag");
  if (api_base_url.empty() || category_id.empty()) {
    *error = "auto mode requires api_base_url and category_id";
    return false;
  }

  IndexInfo index;
  if (!QueryIndex(api_base_url,
                  GetString(request, "branch", "main"),
                  GetString(request, "tag"),
                  previous_tag,
                  &index,
                  error)) {
    return false;
  }
  const CategoryInfo* category = index.Find(category_id);
  if (category == nullptr) {
    *error = "category not present in the index: " + category_id;
    return false;
  }

  *resolved = request;
  (*resolved)["api_base_url"] = api_base_url;
  (*resolved)["category_id"] = category_id;
  (*resolved)["tag"] = index.tag;

  if (category->patch_available && GetBool(request, "prefer_patch", true)) {
    (*resolved)["mode"] = "patch";
    (*resolved)["source_version"] = previous_tag;
    (*resolved)["previous_tag"] = previous_tag;
    return true;
  }

  (*resolved)["mode"] = "chunk";
  (*resolved)["previous_manifest_url"] = std::string();
  (*resolved)["current_manifest_url"] = category->manifest_url;
  (*resolved)["chunk_base_url"] = category->chunk_url_prefix;
  return true;
}

void RunSession(const std::shared_ptr<Session>& session, json request) {
  int code = -1;
  std::string error;
  try {
    std::string mode = GetString(request, "mode", "auto");
    if (mode == "auto") {
      json resolved;
      if (!BuildAutoRequest(request, &resolved, &error)) {
        session->FinishWithError(error);
        return;
      }
      request = std::move(resolved);
      mode = GetString(request, "mode", "chunk");
    }

    if (session->cancel_requested.load()) {
      session->Finish(-1);
      return;
    }

    if (mode == "patch") {
      code = RunPatchDownload(session, request, &error);
    } else if (mode == "chunk") {
      code = RunChunkDownload(session, request, &error);
    } else {
      error = "unsupported mode: " + mode;
    }
  } catch (const std::exception& e) {
    error = std::string("download failed: ") + e.what();
    code = -1;
  } catch (...) {
    error = "download failed with an unknown error";
    code = -1;
  }

  if (code != 0 && !error.empty()) {
    std::lock_guard<std::mutex> lock(session->mutex);
    session->message = error;
  }
  session->Finish(code);
}

// Runs an export body so that a C++ exception can never unwind through the C
// boundary, which would terminate a host that has no unwind tables.
template <typename Body>
int32_t Guard(Body&& body, const char* what) {
  try {
    return body();
  } catch (const std::exception& e) {
    return Fail(std::string(what) + " failed: " + e.what());
  } catch (...) {
    return Fail(std::string(what) + " failed");
  }
}

template <typename Body>
int64_t GuardSession(Body&& body, const char* what) {
  try {
    return body();
  } catch (const std::exception& e) {
    Fail(std::string(what) + " failed: " + e.what());
    return -1;
  } catch (...) {
    Fail(std::string(what) + " failed");
    return -1;
  }
}

}  // namespace

// ============================================================================
// Exported C ABI
// ============================================================================

extern "C" {

int32_t get_build_info_c(char* buffer, int32_t buffer_size) {
  ClearLastError();
  return Guard(
      [&]() {
        return CopyToBuffer(Quaton::get_build_info(), buffer, buffer_size);
      },
      "get_build_info");
}

int32_t quaton_last_error_c(char* buffer, int32_t buffer_size) {
  std::string error;
  {
    std::lock_guard<std::mutex> lock(g_error_mutex);
    error = g_last_error;
  }
  return CopyToBuffer(error, buffer, buffer_size);
}

int32_t quaton_init_c(const char* config_json) {
  ClearLastError();
  return Guard(
      [&]() {
        const char* text = (config_json == nullptr) ? "{}" : config_json;
        json request;
        std::string error;
        if (!ParseRequest(text, &request, &error)) {
          return Fail(error);
        }

        // Validate before touching any global state so a rejected request
        // cannot leave the library half configured.
        const int log_level = GetInt(request, "log_level", 2);
        if (log_level < 0 || log_level > 5) {
          return Fail("log_level must be between 0 and 5");
        }

        const std::string data_dir = GetString(request, "data_dir");
        if (!data_dir.empty()) {
          Quaton::PathHelper::SetDataRoot(data_dir);
        }

        // Logger and LogLevel live in the global namespace (quaton/logger.h).
        ::Logger::setLogLevel(static_cast<LogLevel>(log_level));

        // The library writes into these directories but does not always create
        // them, so a host that points data_dir at a fresh folder would lose the
        // manifest bookkeeping files. Creating the sub directories also creates
        // the data root itself.
        const std::string directories[] = {Quaton::PathHelper::GetDatabaseDir(),
                                           Quaton::PathHelper::GetManifestDir(),
                                           Quaton::PathHelper::GetTempDir()};
        for (const std::string& directory : directories) {
          // Only reject the call when the directory is really unusable: the
          // helper reports failure for some path shapes (UNC shares, for
          // example) even though the directory is already there.
          std::error_code error;
          if (!Quaton::PathHelper::CreateDirectoryRecursive(directory) &&
              !std::filesystem::is_directory(directory, error)) {
            return Fail("cannot create data directory: " + directory);
          }
        }
        return 0;
      },
      "quaton_init");
}

int32_t quaton_shutdown_c(void) {
  ClearLastError();
  return Guard(
      [&]() {
        std::vector<std::shared_ptr<Session>> sessions;
        {
          std::lock_guard<std::mutex> lock(g_sessions_mutex);
          for (auto& entry : g_sessions) {
            sessions.push_back(entry.second);
          }
          g_sessions.clear();
        }
        for (const auto& session : sessions) {
          session->cancel_requested.store(true);
          std::shared_ptr<Quaton::DownloadService> service;
          {
            std::lock_guard<std::mutex> lock(session->mutex);
            service = session->service;
          }
          if (service) {
            service->CancelAll();
          }
        }
        for (const auto& session : sessions) {
          std::lock_guard<std::mutex> lock(session->join_mutex);
          if (session->worker.joinable()) {
            session->worker.join();
            session->joined = true;
          }
        }
        return 0;
      },
      "quaton_shutdown");
}

int32_t quaton_index_query_c(const char* request_json,
                             char* buffer,
                             int32_t buffer_size) {
  ClearLastError();
  return Guard(
      [&]() {
        json request;
        std::string error;
        if (!ParseRequest(request_json, &request, &error)) {
          return Fail(error);
        }

        IndexInfo index;
        if (!QueryIndex(GetString(request, "api_base_url"),
                        GetString(request, "branch", "main"),
                        GetString(request, "tag"),
                        GetString(request, "previous_tag"),
                        &index,
                        &error)) {
          return Fail(error);
        }

        if (!GetBool(request, "include_patch", true)) {
          for (auto& category : index.categories) {
            category.patch_available = false;
            category.patch_manifest_url.clear();
            category.patch_url_prefix.clear();
          }
        }
        return CopyToBuffer(DumpJson(IndexToJson(index)), buffer, buffer_size);
      },
      "quaton_index_query");
}

int64_t quaton_download_start_c(const char* request_json,
                                quaton_progress_callback_c callback,
                                void* user_data) {
  ClearLastError();
  return GuardSession(
      [&]() -> int64_t {
        json request;
        std::string error;
        if (!ParseRequest(request_json, &request, &error)) {
          Fail(error);
          return -1;
        }

        auto session = std::make_shared<Session>();
        session->id = g_next_session_id.fetch_add(1);
        session->callback = callback;
        session->user_data = user_data;
        // The worker starts before the session becomes reachable by id, so a
        // host that releases it immediately always finds a joinable thread.
        session->worker =
            std::thread([session, request]() { RunSession(session, request); });
        RegisterSession(session);
        return session->id;
      },
      "quaton_download_start");
}

int32_t quaton_download_status_c(int64_t session_id,
                                 char* buffer,
                                 int32_t buffer_size) {
  ClearLastError();
  return Guard(
      [&]() {
        const auto session = FindSession(session_id);
        if (session == nullptr) {
          return Fail("unknown session: " + std::to_string(session_id));
        }

        const int state = session->state.load();
        json result = {{"session_id", session_id},
                       {"state", StateName(state)},
                       {"finished", state != kRunning},
                       {"result_code", session->result_code.load()}};
        {
          std::lock_guard<std::mutex> lock(session->mutex);
          result["message"] = session->message;
          result["progress"] = session->progress;
        }
        return CopyToBuffer(DumpJson(result), buffer, buffer_size);
      },
      "quaton_download_status");
}

int32_t quaton_download_cancel_c(int64_t session_id) {
  ClearLastError();
  return Guard(
      [&]() {
        const auto session = FindSession(session_id);
        if (session == nullptr) {
          return Fail("unknown session: " + std::to_string(session_id));
        }
        session->cancel_requested.store(true);
        // Cancelling reaches into the service, which takes its own locks, so
        // the session must not be held while doing it.
        std::shared_ptr<Quaton::DownloadService> service;
        {
          std::lock_guard<std::mutex> lock(session->mutex);
          service = session->service;
        }
        if (service) {
          service->CancelAll();
        }
        return 0;
      },
      "quaton_download_cancel");
}

int32_t quaton_download_release_c(int64_t session_id, int32_t timeout_ms) {
  ClearLastError();
  return Guard(
      [&]() {
        const auto session = FindSession(session_id);
        if (session == nullptr) {
          return Fail("unknown session: " + std::to_string(session_id));
        }
        if (timeout_ms < 0) {
          return Fail("timeout_ms must not be negative");
        }

        {
          std::lock_guard<std::mutex> lock(session->join_mutex);
          if (!session->joined && session->worker.joinable()) {
            if (timeout_ms > 0 && session->state.load() == kRunning) {
              const auto deadline = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(timeout_ms);
              while (session->state.load() == kRunning &&
                     std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
              }
              if (session->state.load() == kRunning) {
                return 1;
              }
            }
            session->worker.join();
            session->joined = true;
          }
        }

        TakeSession(session_id);
        return 0;
      },
      "quaton_download_release");
}

}  // extern "C"
