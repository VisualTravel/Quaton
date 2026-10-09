#ifndef QUATON_QUATON_C_H_
#define QUATON_QUATON_C_H_
#pragma once

#include <stdint.h>

// =============================================================================
// Export macro
// =============================================================================
//
// Mirrors quaton_global.h so a host that loads Quaton.dll through
// LoadLibrary/GetProcAddress can include this header from plain C.

#if defined(QUATON_STATIC)
#define QUATON_C_API
#elif defined(_WIN32) || defined(_WIN64)
#if defined(QUATON_EXPORTS)
#define QUATON_C_API __declspec(dllexport)
#else
#define QUATON_C_API __declspec(dllimport)
#endif
#else
#define QUATON_C_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Calling convention of the progress callback. The exports themselves are
// __cdecl on 32-bit Windows, which is also what a plain function pointer
// without an explicit convention uses.
#if defined(_WIN32) && !defined(_WIN64)
#define QUATON_C_CALL __cdecl
#else
#define QUATON_C_CALL
#endif

// =============================================================================
// Conventions
// =============================================================================
//
// Text results are returned through a caller-owned buffer instead of a
// library-allocated pointer: a host built with a different CRT instance would
// otherwise have to free memory it never allocated.
//
// Call such an export with buffer = NULL to learn the required size, then call
// it again with a buffer of at least that size plus one byte for the NUL
// terminator. The return value is the length in bytes excluding the
// terminator, or a negative value on failure.
//
// JSON inputs and outputs are UTF-8. Unknown JSON members are ignored, so a
// host can keep talking to an older or newer library.
//
// No export throws: a failure is reported through the return value and
// quaton_last_error_c, so the ABI is safe to use from plain C.

// =============================================================================
// Diagnostics
// =============================================================================

/**
 * @brief Library version, platform and dependency versions as text.
 */
QUATON_C_API int32_t get_build_info_c(char* buffer, int32_t buffer_size);

/**
 * @brief Message describing the most recent failed call, empty when the last
 *        call succeeded.
 *
 * Process wide, written by whichever thread failed last. Every other export
 * clears it when it is called, so the value is only meaningful right after a
 * call returned a negative result.
 */
QUATON_C_API int32_t quaton_last_error_c(char* buffer, int32_t buffer_size);

// =============================================================================
// Lifecycle
// =============================================================================

/**
 * @brief Initialize the library for this process.
 *
 * Validates the request before it changes any global state, so a rejected call
 * leaves the library as it was. Failures while creating the data directories
 * can still leave data_dir and log_level applied.
 *
 * @param config_json {"data_dir":"...","log_level":2}
 *        data_dir  root for database, manifests and temp files; when absent the
 *                  platform default is used. Must be set before the first
 *                  download, because it is read when the database is opened.
 *                  The log file is not affected: the logger binds its path when
 *                  the module loads and always writes to the default log
 *                  directory.
 *        log_level 0 trace .. 5 fatal, defaults to info.
 * @return 0 on success, negative on failure
 */
QUATON_C_API int32_t quaton_init_c(const char* config_json);

/**
 * @brief Release library resources. Download sessions must be released first.
 */
QUATON_C_API int32_t quaton_shutdown_c(void);

// =============================================================================
// Index query
// =============================================================================

/**
 * @brief Read the published index for one branch.
 *
 * @param request_json {"api_base_url":"...","branch":"main","tag":"",
 *                      "previous_tag":"","include_patch":true}
 *        tag          target version; empty selects the branch head.
 *        previous_tag enables per-category patch availability.
 * @param buffer       Caller-owned output buffer, may be NULL
 * @param buffer_size  Capacity of buffer in bytes
 * @return Length of the JSON result excluding NUL, negative on failure
 */
QUATON_C_API int32_t quaton_index_query_c(const char* request_json,
                                          char* buffer,
                                          int32_t buffer_size);

// =============================================================================
// Download session
// =============================================================================

/**
 * @brief Reports progress from the download thread; progress_json is only
 *        valid for the duration of the call.
 */
typedef void(QUATON_C_CALL* quaton_progress_callback_c)(
    const char* progress_json, void* user_data);

/**
 * @brief Start an operation session in the background.
 *
 * @param request_json {"mode":"auto","api_base_url":"...","category_id":"...",
 *                      "install_dir":"...","previous_tag":"...",
 *                      "threads":0,"temp_dir":"...","manifest_dir":"...",
 *                      "verify_downloads":true,"silent":false}
 *
 *        mode        "auto"    full or incremental, whichever the index offers
 *                              for previous_tag (requires api_base_url,
 *                              category_id and install_dir)
 *                    "chunk"   plain chunk download of one category from
 *                              explicit manifest URLs
 *                    "patch"   patch-only update
 *                    "verify"  read-only integrity check of an installed
 *                              category against its published manifest; the
 *                              result becomes available through
 *                              quaton_download_result_c once the session has
 *                              finished, and the session itself succeeds even
 *                              when files are reported
 *                    "restore" re-download the files "verify" would report,
 *                              fixing a damaged but present installation
 *        Full chunks additionally accept current_manifest_url,
 *        chunk_base_url, previous_manifest_url, output_dir and filter.
 *        Patches additionally accept patch_manifest_url, patch_manifest_md5,
 *        patch_manifest_compressed, patch_url_prefix, package_name,
 *        source_version and predownload_only; when they are absent the values
 *        are taken from the index.
 *        Verify and restore locate the manifest through api_base_url,
 *        category_id, branch and tag, or through explicit
 *        current_manifest_url and chunk_base_url, and accept max_issues
 *        (verify only, default 5000, 0 collects every failing file).
 *
 *        Queuing several packages: category_ids (an array) replaces
 *        category_id and makes the session work through those categories one
 *        after another, in the given order, reporting their combined progress
 *        (see quaton_download_status_c). "auto", "patch", "verify" and
 *        "restore" accept a list; "chunk" takes explicit manifest URLs and
 *        stays single category. Every category is validated against the index
 *        before the first transfer, a failure stops the queue and names the
 *        category in the session message, and a cancelled queue marks the rest
 *        of the categories "pending".
 *
 *        Nothing is compared against local records: the published manifest is
 *        the only reference, so a tree that this library never downloaded, or
 *        one whose records were removed, is checked just as well. filter is
 *        used by restore only, as the package name the repaired files are
 *        recorded under, and must match the download that installed the tree.
 *
 *        A verify or restore session can be cancelled through
 *        quaton_download_cancel_c; the scan then stops after the file it is
 *        working on.
 *
 * @param callback     Progress sink, may be NULL
 * @param user_data    Passed back to callback untouched
 * @return Session id (> 0) on success, negative on failure
 */
QUATON_C_API int64_t
quaton_download_start_c(const char* request_json,
                        quaton_progress_callback_c callback,
                        void* user_data);

/**
 * @brief Session state and last reported progress as JSON.
 *
 *   {"session_id":1,"state":"running","finished":false,"result_code":0,
 *    "message":"",
 *    "progress":{"mode":"chunk_download","session_mode":"auto",
 *                "percent":0.42,"current_file":"avatar/x.json",
 *                "current_category_id":"textures_hk4e","category_index":2,
 *                "category_count":3,"total_files":13426,
 *                "completed_files":5640,"remaining_files":7786,
 *                "failed_files":0,"speed_mbps":12.5,
 *                "decompress_speed":38.2,"decompressed_bytes":1048576,
 *                "eta_seconds":154.0,
 *                "stages":{"download_initiated":true,...},
 *                "categories":[{"category_id":"game_data_hk4e",
 *                               "state":"succeeded","percent":1.0,
 *                               "total_files":6317,"completed_files":6317,
 *                               "install_size":120397155,
 *                               "download_size":25585068,
 *                               "current_file":""}, ...]}}
 *
 * For a session that names categories, percent, total_files,
 * completed_files and remaining_files cover all of them, weighted by the
 * sizes the index publishes, while current_file, mode and stages belong to the
 * category that is running now. current_category_id, category_index (1 based)
 * and category_count say which one that is, and categories[] carries the
 * per-category state ("pending", "running", "succeeded", "failed",
 * "cancelled") with its own counters, so a host can show both a total bar and
 * the package currently being processed. session_mode is the mode the request
 * asked for, while mode is what the running category is doing.
 *
 * install_size and download_size are the sizes the index publishes and are what
 * the totals are weighted by; they stay 0 when a session works from explicit
 * manifest URLs instead of an index entry, and the file counts are used as
 * weights then. A session publishes one last snapshot when it finishes, so a
 * host that only polls at the end still sees the state it ended in (including a
 * category that stopped on an error) even if no progress callback ever fired.
 *
 * decompress_speed is the payload decompression throughput in MB/s and
 * decompressed_bytes the amount decompressed since the process started; both
 * cover the whole process, because the library decompresses deep inside its
 * workers. They stay 0 for operations that decompress nothing.
 *
 * @return Length of the JSON result excluding NUL, negative on failure
 */
QUATON_C_API int32_t quaton_download_status_c(int64_t session_id,
                                              char* buffer,
                                              int32_t buffer_size);

/**
 * @brief Structured result of a finished session as JSON.
 *
 * Only "verify" produces one. For a single category:
 *
 *   {"mode":"verify","install_dir":"...","total_files":6317,
 *    "valid_files":6313,"missing_files":3,"corrupted_files":1,
 *    "missing_bytes":4096,"corrupted_bytes":8192,
 *    "issues_truncated":false,
 *    "issues":[{"path":"a/b.webp","status":"missing","size":1234,
 *               "actual_size":0,"checksum":"..."}]}
 *
 * status is "missing" when the file is absent and "corrupted" when it exists
 * but differs from the manifest in size or checksum; size is what the manifest
 * lists and actual_size what was found on disk. issues is capped by
 * max_issues, and issues_truncated says whether it was. A "warnings" array is
 * added when no file matched at all, which usually means install_dir holds
 * another build.
 *
 * A verify over several categories carries one report per category plus the
 * totals:
 *
 *   {"mode":"verify","install_dir":"...","category_count":3,
 *    "total_files":13426,"valid_files":13400,"missing_files":26,
 *    "corrupted_files":0,"missing_bytes":...,
 *    "issues_truncated":false,
 *    "categories":[{"category_id":"game_data_hk4e","total_files":6317,
 *                   "valid_files":6317,...,"issues":[...]}, ...]}
 *
 * A session that failed or was cancelled publishes no report and returns {}.
 * Every other mode returns an empty object too.
 *
 * @return Length of the JSON result excluding NUL, negative on failure. A
 *         session that has not finished yet is rejected, so the result is
 *         always complete.
 */
QUATON_C_API int32_t quaton_download_result_c(int64_t session_id,
                                              char* buffer,
                                              int32_t buffer_size);

/**
 * @brief Ask a running session to stop; the session still has to be released.
 *
 * Downloads stop transferring, and a verify or restore scan stops after the
 * file it is working on. A cancelled session reports the state "cancelled".
 */
QUATON_C_API int32_t quaton_download_cancel_c(int64_t session_id);

/**
 * @brief Wait for a session to finish and drop it.
 *
 * @param timeout_ms 0 waits indefinitely, otherwise the maximum time to wait;
 *                   negative values are rejected
 * @return 0 when the session finished, 1 when the wait timed out (the session
 *         stays valid and can be released again), negative on failure
 */
QUATON_C_API int32_t quaton_download_release_c(int64_t session_id,
                                               int32_t timeout_ms);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // QUATON_QUATON_C_H_
