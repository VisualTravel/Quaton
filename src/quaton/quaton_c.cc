// C ABI for hosts that consume Quaton through LoadLibrary instead of the
// import library. Nothing crosses the boundary except plain data: results are
// copied into caller-owned buffers and every structured value is JSON, so a
// host built against a different CRT never frees Quaton memory.

#include "quaton/quaton_c.h"

#include <algorithm>
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

class ProgressAggregator;  // defined with the progress plumbing below

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

  // guards message, progress, last_progress, result, service and cancel
  std::mutex mutex;
  std::string message;
  json progress = json::object();
  // Last sample a worker reported, kept so the closing snapshot of a session
  // describes the run that just happened instead of a blank default.
  Quaton::ProgressInfo last_progress;
  json result = json::object();
  std::shared_ptr<Quaton::DownloadService> service;
  // Published by the workers that scan the disk, because those go through
  // neither the scheduler nor a session cancel flag of their own.
  std::shared_ptr<std::atomic<bool>> cancel_flag;
  // Set while a session works through several categories, so that the progress
  // payload carries their combined view (defined below).
  std::shared_ptr<ProgressAggregator> aggregator;

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

// The workers that scan the install directory do not run through the scheduler,
// so they publish their own flag for quaton_download_cancel_c to set.
std::shared_ptr<std::atomic<bool>> PublishCancelFlag(
    const std::shared_ptr<Session>& session) {
  auto flag = std::make_shared<std::atomic<bool>>(false);
  std::lock_guard<std::mutex> lock(session->mutex);
  // A cancellation that arrived while the session was still preparing must not
  // be lost, because nothing looks at cancel_requested again afterwards.
  flag->store(session->cancel_requested.load());
  session->cancel_flag = flag;
  return flag;
}

// Stops a session wherever it currently is: a download through the service, a
// scan through the flag the scan published.
void RequestSessionCancel(const std::shared_ptr<Session>& session) {
  session->cancel_requested.store(true);
  // Cancelling reaches into the service, which takes its own locks, so the
  // session must not be held while doing it.
  std::shared_ptr<Quaton::DownloadService> service;
  std::shared_ptr<std::atomic<bool>> cancel_flag;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    service = session->service;
    cancel_flag = session->cancel_flag;
  }
  if (cancel_flag) {
    cancel_flag->store(true);
  }
  if (service) {
    service->CancelAll();
  }
}

// ============================================================================
// Progress reporting
// ============================================================================

const char* OperationModeName(Quaton::OperationMode mode) {
  switch (mode) {
    case Quaton::OperationMode::kChunkVerify:
      // Same name as the session mode and the report mode, so a host can match
      // progress to the result without a second mapping.
      return "verify";
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
          {"decompress_speed", info.decompression_speed},
          {"decompressed_bytes", info.decompressed_bytes},
          {"eta_seconds", info.estimated_seconds},
          {"stages",
           {{"download_initiated", status.download_initiated},
            {"download_completed", status.download_completed},
            {"decompressed", status.decompressed},
            {"verified", status.verified},
            {"applied", status.applied}}}};
}

// What one category of a multi-category session is doing.
enum CategoryState {
  kCategoryPending = 0,
  kCategoryRunning = 1,
  kCategorySucceeded = 2,
  kCategoryFailed = 3,
  kCategoryCancelled = 4
};

const char* CategoryStateName(int state) {
  switch (state) {
    case kCategoryRunning:
      return "running";
    case kCategorySucceeded:
      return "succeeded";
    case kCategoryFailed:
      return "failed";
    case kCategoryCancelled:
      return "cancelled";
    default:
      return "pending";
  }
}

struct CategoryRun {
  std::string category_id;
  int64_t install_size = 0;   ///< Bytes on disk, used to weight the percentages
  int64_t download_size = 0;  ///< Bytes to transfer
  int total_files = 0;        ///< File count the index publishes
  int reported_files = 0;     ///< File count the running service reports
  int completed_files = 0;
  double percent = 0.0;  ///< Last percentage the category reported
  std::string current_file;
  int state = kCategoryPending;
};

// Turns the progress of the category that is currently running into the
// combined progress a host sees, so a queued session can still name the
// category and the file it is working on.
class ProgressAggregator {
 public:
  ProgressAggregator(std::vector<std::string> category_ids,
                     std::string session_mode)
      : session_mode_(std::move(session_mode)) {
    runs_.reserve(category_ids.size());
    for (auto& id : category_ids) {
      CategoryRun run;
      run.category_id = std::move(id);
      runs_.push_back(std::move(run));
    }
  }

  void SetCategoryInfo(size_t index,
                       int64_t install_size,
                       int64_t download_size,
                       int total_files) {
    std::lock_guard<std::mutex> lock(mutex_);
    CategoryRun& run = runs_.at(index);
    run.install_size = install_size;
    run.download_size = download_size;
    run.total_files = total_files;
  }

  void Begin(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = index;
    runs_.at(index).state = kCategoryRunning;
  }

  void Finish(size_t index, int state) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = index;
    CategoryRun& run = runs_.at(index);
    run.state = state;
    if (state == kCategorySucceeded) {
      run.percent = 1.0;
      run.current_file.clear();
      run.completed_files = CategoryFileCount(run);
    }
  }

  /**
   * @brief Adds the combined view of every category to one progress payload
   */
  json Merge(const json& payload, const Quaton::ProgressInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);

    CategoryRun& active = runs_.at(active_);
    if (active.state == kCategoryRunning) {
      active.reported_files = info.total_files;
      active.completed_files = info.completed_files;
      active.percent = std::max(0.0, std::min(1.0, info.overall_percentage));
      active.current_file = info.current_file;
    }

    int64_t total_weight = 0;
    double done_weight = 0.0;
    int64_t remaining_download_bytes = 0;
    int total_files = 0;
    int completed_files = 0;
    json categories = json::array();

    for (const CategoryRun& run : runs_) {
      // The index knows the size before the category starts, so the weighting
      // stays stable for the whole session. The file count only stands in when
      // the index published no size at all; the running count is deliberately
      // not used, because it would move the total mid-session.
      int64_t weight = run.install_size;
      if (weight <= 0) {
        weight = run.download_size;
      }
      if (weight <= 0) {
        weight = run.total_files;
      }
      if (weight <= 0) {
        weight = 1;
      }

      const int file_count = CategoryFileCount(run);
      total_weight += weight;
      done_weight += static_cast<double>(weight) * run.percent;
      // The download speed counts transferred (compressed) bytes, so the
      // estimate is derived from the transfer size, not the installed size.
      if (run.percent < 1.0 && run.download_size > 0) {
        remaining_download_bytes += static_cast<int64_t>(
            static_cast<double>(run.download_size) * (1.0 - run.percent));
      }
      total_files += file_count;
      completed_files += run.completed_files;

      categories.push_back({{"category_id", run.category_id},
                            {"state", CategoryStateName(run.state)},
                            {"percent", run.percent},
                            {"total_files", file_count},
                            {"completed_files", run.completed_files},
                            {"install_size", run.install_size},
                            {"download_size", run.download_size},
                            {"current_file", run.current_file}});
    }

    json merged = payload;
    merged["session_mode"] = session_mode_;
    merged["percent"] = total_weight > 0
                            ? done_weight / static_cast<double>(total_weight)
                            : 0.0;
    merged["total_files"] = total_files;
    merged["completed_files"] = completed_files;
    merged["remaining_files"] = std::max(0, total_files - completed_files);
    merged["category_count"] = static_cast<int>(runs_.size());
    merged["category_index"] = static_cast<int>(active_ + 1);
    merged["current_category_id"] = active.category_id;
    merged["categories"] = std::move(categories);

    // Chunk downloads report no byte totals, so the estimate is derived from
    // the transfer sizes the index publishes.
    const double speed_bytes = info.download_speed * 1024.0 * 1024.0;
    if (speed_bytes > 0.0 && remaining_download_bytes > 0) {
      merged["eta_seconds"] = remaining_download_bytes / speed_bytes;
    }
    return merged;
  }

 private:
  /// Files a category holds: the running count wins over the published one.
  static int CategoryFileCount(const CategoryRun& run) {
    return std::max(run.total_files, run.reported_files);
  }

  const std::string session_mode_;
  std::mutex mutex_;  // guards runs_ and active_
  std::vector<CategoryRun> runs_;
  size_t active_ = 0;
};

// Keeps the session's aggregator alive for the length of a queued run and
// clears it on every exit path.
class AggregatorScope {
 public:
  AggregatorScope(const std::shared_ptr<Session>& session,
                  std::shared_ptr<ProgressAggregator> aggregator)
      : session_(session) {
    std::lock_guard<std::mutex> lock(session_->mutex);
    session_->aggregator = std::move(aggregator);
  }

  ~AggregatorScope() {
    std::lock_guard<std::mutex> lock(session_->mutex);
    session_->aggregator.reset();
  }

  AggregatorScope(const AggregatorScope&) = delete;
  AggregatorScope& operator=(const AggregatorScope&) = delete;

 private:
  std::shared_ptr<Session> session_;
};

// Called from the library's download threads.
void ReportProgress(const std::shared_ptr<Session>& session,
                    const Quaton::ProgressInfo& info) {
  json payload = ProgressToJson(info);
  std::shared_ptr<ProgressAggregator> aggregator;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    aggregator = session->aggregator;
  }
  if (aggregator) {
    payload = aggregator->Merge(payload, info);
  }

  quaton_progress_callback_c callback = nullptr;
  void* user_data = nullptr;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    session->progress = payload;
    session->last_progress = info;
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

// The closing snapshot a session publishes once it is done. It starts from the
// last sample a worker reported, so a host that polls only at the end still
// sees the measured speeds and byte counts of the run; a session that never got
// a callback (a very small package) falls back to the defaults.
Quaton::ProgressInfo SessionSummary(const std::shared_ptr<Session>& session) {
  std::lock_guard<std::mutex> lock(session->mutex);
  Quaton::ProgressInfo info;
  info = session->last_progress;
  // The session is over, so no file is being worked on any more.
  info.current_file.clear();
  return info;
}

// Holds the running service for the duration of a download. The progress
// callback captures the session, so the service would keep the session alive in
// return; dropping the session's reference here destroys the service, which in
// turn destroys the callback that captured the session, and that is what breaks
// the cycle. It has to happen on every exit path.
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
// Shared request plumbing for the download, verify and restore workers
// ============================================================================

// The install directory accepts the output_dir alias so a host can use either
// name for the same thing.
std::string RequestInstallDir(const json& request) {
  std::string install_dir = GetString(request, "install_dir");
  if (install_dir.empty()) {
    install_dir = GetString(request, "output_dir");
  }
  return install_dir;
}

// Reads the categories a request works on: category_ids (array) wins over the
// single category_id, and the request is normalised so that one category keeps
// behaving exactly as it did before.
std::vector<std::string> RequestCategoryIds(json* request) {
  std::vector<std::string> ids;
  const auto it = request->find("category_ids");
  if (it != request->end() && it->is_array()) {
    for (const auto& entry : *it) {
      if (!entry.is_string()) {
        continue;
      }
      const std::string id = entry.get<std::string>();
      if (!id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end()) {
        ids.push_back(id);
      }
    }
  }
  if (ids.empty()) {
    const std::string single = GetString(*request, "category_id");
    if (!single.empty()) {
      ids.push_back(single);
    }
    return ids;
  }
  (*request)["category_id"] = ids.size() == 1 ? ids.front() : std::string();
  return ids;
}

// The version a patch update is built from; a host may name it either way.
std::string RequestSourceVersion(const json& request) {
  return GetString(
      request, "source_version", GetString(request, "previous_tag"));
}

// Verification and repair read a tree that a download already installed, so a
// missing directory is a caller mistake worth naming rather than a bare -1.
bool RequireInstallDir(const std::string& install_dir,
                       const char* mode,
                       std::string* error) {
  if (install_dir.empty()) {
    *error = std::string(mode) + " mode requires install_dir";
    return false;
  }
  std::error_code code;
  if (!std::filesystem::is_directory(install_dir, code)) {
    *error = std::string(mode) +
             " mode needs an existing install_dir: " + install_dir;
    return false;
  }
  return true;
}

Quaton::ChunkServiceConfig BuildChunkConfig(const json& request,
                                            const std::string& install_dir) {
  Quaton::ChunkServiceConfig config;
  config.thread_count = GetInt(request, "threads", 0);
  config.max_http_handles = GetInt(request, "max_http_handles", 128);
  config.retry_count = GetInt(request, "retry_count", 5);
  config.verify_downloads = GetBool(request, "verify_downloads", true);
  config.silent = GetBool(request, "silent", false);
  config.install_path = install_dir;
  config.temp_path = GetString(request, "temp_dir");
  config.manifest_output_dir = GetString(request, "manifest_dir");
  return config;
}

// A manifest file URL carries no integrity metadata, so checksum and size are
// placeholders; enumeration only needs the base URL and the file name.
bool MakeManifestPair(const std::string& manifest_url,
                      const std::string& chunk_base_url,
                      Quaton::ChunkManifestPair* manifest_pair,
                      std::string* error) {
  const auto slash = manifest_url.find_last_of('/');
  if (slash == std::string::npos || slash + 1 >= manifest_url.size()) {
    *error = "invalid manifest URL: " + manifest_url;
    return false;
  }

  manifest_pair->set_manifest_metadata(
      Quaton::ManifestMetadata(manifest_url.substr(0, slash),
                               "url",
                               manifest_url.substr(slash + 1),
                               /*use_compression=*/true,
                               1,
                               0));
  manifest_pair->get_chunk_info().set_base_url(chunk_base_url);
  return true;
}

// Resolves the manifest a verification or a repair works on: either the URLs
// come straight from the request, or they are taken from the index for one
// category. Both paths yield the same manifest identity a download of that
// category used, which is what makes the recorded file keys comparable.
bool ResolveManifestTarget(const json& request,
                           std::string* manifest_url,
                           std::string* chunk_base_url,
                           std::string* error) {
  *manifest_url = GetString(request, "current_manifest_url");
  *chunk_base_url = GetString(request, "chunk_base_url");
  if (!manifest_url->empty() && !chunk_base_url->empty()) {
    return true;
  }

  const std::string category_id = GetString(request, "category_id");
  const std::string api_base_url = GetString(request, "api_base_url");
  if (api_base_url.empty() || category_id.empty()) {
    *error =
        "api_base_url and category_id, or current_manifest_url and "
        "chunk_base_url, are required";
    return false;
  }

  IndexInfo index;
  if (!QueryIndex(api_base_url,
                  GetString(request, "branch", "main"),
                  GetString(request, "tag"),
                  std::string(),
                  &index,
                  error)) {
    return false;
  }

  const CategoryInfo* category = index.Find(category_id);
  if (category == nullptr) {
    *error = "category not present in the index: " + category_id;
    return false;
  }
  if (manifest_url->empty()) {
    *manifest_url = category->manifest_url;
  }
  if (chunk_base_url->empty()) {
    *chunk_base_url = category->chunk_url_prefix;
  }
  return true;
}

json VerifyReportToJson(
    const Quaton::ChunkDownloadService::LocalVerifyReport& report,
    const std::string& install_dir,
    const std::string& category_id) {
  json issues = json::array();
  for (const auto& issue : report.issues) {
    issues.push_back({{"path", issue.file_name},
                      {"status", issue.missing ? "missing" : "corrupted"},
                      {"size", issue.size},
                      {"actual_size", issue.actual_size},
                      {"checksum", issue.checksum}});
  }

  json result = {{"mode", "verify"},
                 {"install_dir", install_dir},
                 {"total_files", report.total_files},
                 {"valid_files", report.valid_files},
                 {"missing_files", report.missing_files},
                 {"corrupted_files", report.corrupted_files},
                 {"missing_bytes", report.missing_bytes},
                 {"corrupted_bytes", report.corrupted_bytes},
                 {"issues_truncated", report.issues_truncated},
                 {"issues", std::move(issues)}};
  if (!category_id.empty()) {
    result["category_id"] = category_id;
  }
  // Not a single file matched the manifest, which usually means the directory
  // belongs to another build rather than that every file is damaged.
  if (report.total_files > 0 && report.valid_files == 0) {
    result["warnings"] = json::array(
        {"no file matched the manifest: install_dir does not hold the files "
         "this manifest describes"});
  }
  return result;
}

// ============================================================================
// Download workers
// ============================================================================

int RunChunkDownload(const std::shared_ptr<Session>& session,
                     const json& request,
                     std::string* error) {
  const std::string manifest_url = GetString(request, "current_manifest_url");
  const std::string chunk_url = GetString(request, "chunk_base_url");
  const std::string install_dir = RequestInstallDir(request);
  if (manifest_url.empty() || chunk_url.empty() || install_dir.empty()) {
    *error =
        "chunk mode requires current_manifest_url, chunk_base_url and "
        "install_dir";
    return -1;
  }

  auto service = std::make_shared<Quaton::ChunkDownloadService>();
  service->SetProgressCallback([session](const Quaton::ProgressInfo& info) {
    ReportProgress(session, info);
  });

  if (!service->Initialize(BuildChunkConfig(request, install_dir))) {
    *error = "chunk download service failed to initialize";
    return -1;
  }

  // Published only once the service is initialized, so that a concurrent cancel
  // cannot reach into a half-built scheduler.
  ServiceOwnership ownership(session, service);

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

// Reads the installed tree back against the manifest and stores the report in
// the session. Nothing is written to the install directory, so a tree with
// problems still counts as a finished scan.
int RunChunkVerify(const std::shared_ptr<Session>& session,
                   const json& request,
                   std::string* error) {
  const std::string install_dir = RequestInstallDir(request);
  if (!RequireInstallDir(install_dir, "verify", error)) {
    return -1;
  }

  std::string manifest_url;
  std::string chunk_base_url;
  if (!ResolveManifestTarget(request, &manifest_url, &chunk_base_url, error)) {
    return -1;
  }

  Quaton::ChunkManifestPair manifest_pair;
  if (!MakeManifestPair(manifest_url, chunk_base_url, &manifest_pair, error)) {
    return -1;
  }

  auto service = std::make_shared<Quaton::ChunkDownloadService>();
  service->SetProgressCallback([session](const Quaton::ProgressInfo& info) {
    ReportProgress(session, info);
  });

  if (!service->Initialize(BuildChunkConfig(request, install_dir))) {
    *error = "verify service failed to initialize";
    return -1;
  }

  // The service is published only once it is initialized, so a concurrent
  // cancel cannot reach into a half-built scheduler.
  ServiceOwnership ownership(session, service);

  Quaton::ChunkDownloadService::LocalVerifyReport report;
  const int max_issues = GetInt(request, "max_issues", 5000);
  if (max_issues < 0) {
    *error = "max_issues must not be negative";
    return -1;
  }
  report.max_issues = static_cast<size_t>(max_issues);

  if (!service->Verify(
          manifest_pair, install_dir, &report, PublishCancelFlag(session))) {
    if (report.cancelled) {
      // A cancelled scan has no report to publish and must not surface as a
      // failure with a misleading reason.
      session->cancel_requested.store(true);
      *error = "integrity check cancelled";
    } else {
      *error = "cannot check the install directory";
    }
    return -1;
  }

  {
    std::lock_guard<std::mutex> lock(session->mutex);
    session->result = VerifyReportToJson(
        report, install_dir, GetString(request, "category_id"));
  }
  return 0;
}

// Downloads whatever the same check reports as missing or corrupted.
int RunRestore(const std::shared_ptr<Session>& session,
               const json& request,
               std::string* error) {
  const std::string install_dir = RequestInstallDir(request);
  if (!RequireInstallDir(install_dir, "restore", error)) {
    return -1;
  }

  std::string manifest_url;
  std::string chunk_base_url;
  if (!ResolveManifestTarget(request, &manifest_url, &chunk_base_url, error)) {
    return -1;
  }

  Quaton::ChunkManifestPair manifest_pair;
  if (!MakeManifestPair(manifest_url, chunk_base_url, &manifest_pair, error)) {
    return -1;
  }

  auto service = std::make_shared<Quaton::ChunkDownloadService>();
  service->SetProgressCallback([session](const Quaton::ProgressInfo& info) {
    ReportProgress(session, info);
  });

  if (!service->Initialize(BuildChunkConfig(request, install_dir))) {
    *error = "restore service failed to initialize";
    return -1;
  }

  // Published only after Initialize, for the same reason as in verify.
  ServiceOwnership ownership(session, service);

  // filter is the package identity the download used, and the repaired files
  // are recorded under the same name.
  const std::string package = GetString(request, "filter");
  const auto cancel_flag = PublishCancelFlag(session);
  const int code = service
                       ->Restore(manifest_pair,
                                 install_dir,
                                 package.empty() ? "game" : package,
                                 cancel_flag)
                       .get();
  if (code != 0) {
    if (cancel_flag->load()) {
      session->cancel_requested.store(true);
      *error = "repair cancelled";
    } else {
      *error = "restore returned " + std::to_string(code);
    }
  }
  return code;
}

int RunPatchDownload(const std::shared_ptr<Session>& session,
                     const json& request,
                     std::string* error) {
  std::string install_dir = RequestInstallDir(request);
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
  service->SetProgressCallback([session](const Quaton::ProgressInfo& info) {
    ReportProgress(session, info);
  });

  if (!service->Initialize(config)) {
    *error = "patch download service failed to initialize";
    return -1;
  }

  // Published only once the service is initialized, so that a concurrent cancel
  // cannot reach into a half-built scheduler.
  ServiceOwnership ownership(session, service);

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
  const std::string previous_tag = RequestSourceVersion(request);
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

// Fills in everything one category of a queued session needs, from an index
// that was already fetched, so the queue does not query the index once per
// category and the workers never fall back to a second lookup.
bool ResolveUnitRequest(const json& request,
                        const IndexInfo& index,
                        const CategoryInfo& category,
                        const std::string& mode,
                        json* unit,
                        std::string* error) {
  *unit = request;
  (*unit)["category_id"] = category.category_id;
  (*unit)["tag"] = index.tag;

  if (mode == "verify" || mode == "restore") {
    (*unit)["current_manifest_url"] = category.manifest_url;
    (*unit)["chunk_base_url"] = category.chunk_url_prefix;
    return true;
  }

  const std::string previous_tag = RequestSourceVersion(request);
  const bool wants_patch =
      mode == "patch" || (mode == "auto" && category.patch_available &&
                          GetBool(request, "prefer_patch", true));
  if (!wants_patch) {
    (*unit)["mode"] = "chunk";
    (*unit)["previous_manifest_url"] = std::string();
    (*unit)["current_manifest_url"] = category.manifest_url;
    (*unit)["chunk_base_url"] = category.chunk_url_prefix;
    return true;
  }

  if (!category.patch_available) {
    *error = "no patch set for source version: " + previous_tag;
    return false;
  }
  (*unit)["mode"] = "patch";
  (*unit)["source_version"] = previous_tag;
  (*unit)["previous_tag"] = previous_tag;
  (*unit)["patch_manifest_url"] = category.patch_manifest_url;
  (*unit)["patch_manifest_id"] = category.patch_manifest_id;
  (*unit)["patch_manifest_md5"] = category.patch_manifest_checksum;
  (*unit)["patch_url_prefix"] = category.patch_url_prefix;
  (*unit)["package_name"] = category.category_id;
  return true;
}

bool IsKnownQueueMode(const std::string& mode) {
  return mode == "auto" || mode == "patch" || mode == "verify" ||
         mode == "restore";
}

/// Operation the first category of a queue starts with, used for the snapshot
/// published before any transfer begins.
Quaton::OperationMode QueueOperationMode(const std::string& mode) {
  if (mode == "patch") {
    return Quaton::OperationMode::kPatchUpdate;
  }
  if (mode == "verify" || mode == "restore") {
    return Quaton::OperationMode::kChunkVerify;
  }
  return Quaton::OperationMode::kChunkDownload;
}

// Runs the categories one after another inside a single session and reports
// their combined progress. Sequential is deliberate: the caller asked for one
// total progress over one install directory, and a host that wants transfers
// in parallel can still start several sessions.
int RunCategoryQueue(const std::shared_ptr<Session>& session,
                     const json& request,
                     const std::string& mode,
                     const std::vector<std::string>& category_ids,
                     std::string* error) {
  const std::string api_base_url = GetString(request, "api_base_url");
  if (api_base_url.empty()) {
    *error = mode + " mode with several categories requires api_base_url";
    return -1;
  }

  IndexInfo index;
  if (!QueryIndex(api_base_url,
                  GetString(request, "branch", "main"),
                  GetString(request, "tag"),
                  RequestSourceVersion(request),
                  &index,
                  error)) {
    return -1;
  }
  // Every category is checked before the first byte is transferred, so a typo
  // cannot leave a half updated installation behind.
  for (const std::string& category_id : category_ids) {
    if (index.Find(category_id) == nullptr) {
      *error = "category not present in the index: " + category_id;
      return -1;
    }
  }

  auto aggregator = std::make_shared<ProgressAggregator>(category_ids, mode);
  for (size_t i = 0; i < category_ids.size(); ++i) {
    const CategoryInfo* category = index.Find(category_ids[i]);
    aggregator->SetCategoryInfo(i,
                                category->uncompressed_size,
                                category->compressed_size,
                                category->file_count);
  }
  AggregatorScope scope(session, aggregator);

  // Publish the queue itself before the first transfer, so a host can show
  // "1/7 bhyp" without waiting for a progress callback of the first category.
  aggregator->Begin(0);
  {
    Quaton::ProgressInfo initial;
    initial.operation_mode = QueueOperationMode(mode);
    ReportProgress(session, initial);
  }

  const auto clear_result = [&session]() {
    std::lock_guard<std::mutex> lock(session->mutex);
    session->result = json::object();
  };
  clear_result();

  // The mode the last category actually ran, so an "auto" queue that turned
  // into a patch update reports that rather than a plain download.
  std::string unit_mode = mode;
  // One last snapshot per exit path, so what the host polls describes the state
  // the session ended in, including a category that stopped on an error or
  // never got a progress callback of its own.
  const auto publish_summary = [&session, &unit_mode]() {
    Quaton::ProgressInfo done = SessionSummary(session);
    done.operation_mode = QueueOperationMode(unit_mode);
    ReportProgress(session, done);
  };
  const auto clear_queue_result =
      [&session, &clear_result, &publish_summary]() {
        clear_result();
        publish_summary();
      };

  json reports = json::array();
  for (size_t i = 0; i < category_ids.size(); ++i) {
    if (session->cancel_requested.load()) {
      aggregator->Finish(i, kCategoryCancelled);
      clear_queue_result();
      *error = "cancelled";
      return -1;
    }
    aggregator->Begin(i);

    json unit;
    if (!ResolveUnitRequest(
            request, index, *index.Find(category_ids[i]), mode, &unit, error)) {
      aggregator->Finish(i, kCategoryFailed);
      clear_queue_result();
      return -1;
    }

    unit_mode = mode == "auto" ? GetString(unit, "mode", "chunk") : mode;
    std::string unit_error;
    int unit_code = -1;
    if (unit_mode == "patch") {
      unit_code = RunPatchDownload(session, unit, &unit_error);
    } else if (unit_mode == "chunk") {
      unit_code = RunChunkDownload(session, unit, &unit_error);
    } else if (unit_mode == "verify") {
      unit_code = RunChunkVerify(session, unit, &unit_error);
    } else if (unit_mode == "restore") {
      unit_code = RunRestore(session, unit, &unit_error);
    } else {
      unit_error = "unsupported mode: " + unit_mode;
    }

    if (unit_code != 0) {
      aggregator->Finish(i,
                         session->cancel_requested.load() ? kCategoryCancelled
                                                          : kCategoryFailed);
      clear_queue_result();
      *error = "category " + category_ids[i] + ": " +
               (unit_error.empty() ? "failed" : unit_error);
      return unit_code;
    }
    aggregator->Finish(i, kCategorySucceeded);

    if (unit_mode == "verify") {
      json report;
      {
        std::lock_guard<std::mutex> lock(session->mutex);
        report = session->result;
      }
      report["category_id"] = category_ids[i];
      reports.push_back(std::move(report));
    }
  }

  if (mode == "verify") {
    // One report per category plus the totals a caller wants at a glance.
    json merged = {{"mode", "verify"},
                   {"install_dir", RequestInstallDir(request)},
                   {"category_count", static_cast<int>(category_ids.size())},
                   {"categories", std::move(reports)}};
    bool truncated = false;
    for (const char* key : {"total_files",
                            "valid_files",
                            "missing_files",
                            "corrupted_files",
                            "missing_bytes",
                            "corrupted_bytes"}) {
      int64_t sum = 0;
      for (const auto& report : merged["categories"]) {
        sum += report.value(key, int64_t{0});
      }
      merged[key] = sum;
    }
    for (const auto& report : merged["categories"]) {
      truncated = truncated || report.value("issues_truncated", false);
    }
    merged["issues_truncated"] = truncated;
    if (merged["total_files"].get<int64_t>() > 0 &&
        merged["valid_files"].get<int64_t>() == 0) {
      merged["warnings"] = json::array(
          {"no file matched the manifests: install_dir does not hold the files "
           "they describe"});
    }
    std::lock_guard<std::mutex> lock(session->mutex);
    session->result = std::move(merged);
  }

  // One last snapshot, so a category that finished without a single progress
  // callback still shows up as done in what the host polls.
  publish_summary();
  return 0;
}

// Runs the single category a request names (or none, when the request carries
// explicit manifest URLs). An aggregator with one entry keeps the progress
// payload shaped the same as a queued session.
int RunSingleCategory(const std::shared_ptr<Session>& session,
                      const json& request,
                      const std::string& mode,
                      const std::vector<std::string>& category_ids,
                      std::string* error) {
  auto aggregator = category_ids.empty() ? nullptr
                                         : std::make_shared<ProgressAggregator>(
                                               category_ids, mode);
  AggregatorScope scope(session, aggregator);
  if (aggregator) {
    aggregator->Begin(0);
  }

  // The mode the session actually resolved to, which is what the closing
  // snapshot publishes.
  std::string resolved_mode = mode;
  const auto publish_summary = [&session, &resolved_mode]() {
    Quaton::ProgressInfo done = SessionSummary(session);
    done.operation_mode = QueueOperationMode(resolved_mode);
    ReportProgress(session, done);
  };

  json resolved = request;
  if (mode == "auto") {
    if (!BuildAutoRequest(request, &resolved, error)) {
      if (aggregator) {
        aggregator->Finish(0, kCategoryFailed);
      }
      publish_summary();
      return -1;
    }
    resolved_mode = GetString(resolved, "mode", "chunk");
  }

  if (session->cancel_requested.load()) {
    if (aggregator) {
      aggregator->Finish(0, kCategoryCancelled);
    }
    *error = "cancelled";
    publish_summary();
    return -1;
  }

  int code = -1;
  if (resolved_mode == "patch") {
    code = RunPatchDownload(session, resolved, error);
  } else if (resolved_mode == "chunk") {
    code = RunChunkDownload(session, resolved, error);
  } else if (resolved_mode == "verify") {
    code = RunChunkVerify(session, resolved, error);
  } else if (resolved_mode == "restore") {
    code = RunRestore(session, resolved, error);
  } else {
    *error = "unsupported mode: " + resolved_mode;
  }

  if (aggregator) {
    aggregator->Finish(
        0,
        code == 0 ? kCategorySucceeded
                  : (session->cancel_requested.load() ? kCategoryCancelled
                                                      : kCategoryFailed));
  }
  // One last snapshot, so a package that finished without a single progress
  // callback still shows up as done in what the host polls.
  publish_summary();
  return code;
}

void RunSession(const std::shared_ptr<Session>& session, json request) {
  int code = -1;
  std::string error;
  try {
    const std::string mode = GetString(request, "mode", "auto");
    const std::vector<std::string> category_ids = RequestCategoryIds(&request);

    if (category_ids.size() > 1) {
      if (mode == "chunk") {
        error =
            "chunk mode downloads explicit manifest URLs and takes one "
            "category; use auto or patch to queue several";
      } else if (!IsKnownQueueMode(mode)) {
        error = "unsupported mode: " + mode;
      } else {
        code = RunCategoryQueue(session, request, mode, category_ids, &error);
      }
    } else {
      code = RunSingleCategory(session, request, mode, category_ids, &error);
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
          RequestSessionCancel(session);
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

int32_t quaton_download_result_c(int64_t session_id,
                                 char* buffer,
                                 int32_t buffer_size) {
  ClearLastError();
  return Guard(
      [&]() {
        const auto session = FindSession(session_id);
        if (session == nullptr) {
          return Fail("unknown session: " + std::to_string(session_id));
        }
        if (session->state.load() == kRunning) {
          return Fail("session is still running: " +
                      std::to_string(session_id));
        }

        json result;
        {
          std::lock_guard<std::mutex> lock(session->mutex);
          result = session->result;
        }
        return CopyToBuffer(DumpJson(result), buffer, buffer_size);
      },
      "quaton_download_result");
}

int32_t quaton_download_cancel_c(int64_t session_id) {
  ClearLastError();
  return Guard(
      [&]() {
        const auto session = FindSession(session_id);
        if (session == nullptr) {
          return Fail("unknown session: " + std::to_string(session_id));
        }
        RequestSessionCancel(session);
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
