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
 * leaves the library as it was.
 *
 * @param config_json {"data_dir":"...","log_level":2}
 *        data_dir  root for database, manifests, temp and log files; when
 *                  absent the platform default is used. Must be set before
 *                  the first download, because it is read when the database is
 *                  opened.
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
 * @brief Start a download or update in the background.
 *
 * @param request_json {"mode":"auto","api_base_url":"...","category_id":"...",
 *                      "install_dir":"...","previous_tag":"...",
 *                      "threads":0,"temp_dir":"...","manifest_dir":"...",
 *                      "verify_downloads":true,"silent":false}
 *
 *        mode        "auto"   full or incremental, whichever the index offers
 *                             for previous_tag (requires api_base_url,
 *                             category_id and install_dir)
 *                    "chunk"  plain chunk download of one category from
 *                             explicit manifest URLs
 *                    "patch"  patch-only update
 *        Full chunks additionally accept current_manifest_url,
 *        chunk_base_url, previous_manifest_url, output_dir and filter.
 *        Patches additionally accept patch_manifest_url, patch_manifest_md5,
 *        patch_manifest_compressed, patch_url_prefix, package_name,
 *        source_version and predownload_only; when they are absent the values
 *        are taken from the index.
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
 * @return Length of the JSON result excluding NUL, negative on failure
 */
QUATON_C_API int32_t quaton_download_status_c(int64_t session_id,
                                              char* buffer,
                                              int32_t buffer_size);

/**
 * @brief Ask a running session to stop; the session still has to be released.
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
