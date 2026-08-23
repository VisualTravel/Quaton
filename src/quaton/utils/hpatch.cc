#include "quaton/utils/hpatch.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "quaton/logger.h"

// Enable the zstd decompress and xxh128 checksum plugins bundled with
// HDiffPatch (MIT license, vendored in third_party/HDiffPatch).
#define _CompressPlugin_zstd
#define _ChecksumPlugin_xxh128
#include "checksum_plugin_demo.h"
#include "decompress_plugin_demo.h"
#include "file_for_patch.h"
#include "libHDiffPatch/HPatch/patch.h"

namespace fs = std::filesystem;

namespace Quaton {
namespace HPatchUtils {

namespace {

// Sample of the patch head used to pick the driving API. hpatchz routes
// HDIFF13 (hdiffz v4, used by the packager) through patch_decompress, and
// HDIFFW26 (hdiffz v5 window diff) through patch_window_diff.
hpatch_BOOL GetDiffInfo(hpatch_compressedDiffInfo* out_info,
                        hpatch_windowDiffInfo* out_win,
                        hpatch_BOOL* is_window,
                        const hpatch_TStreamInput* diff) {
  if (getCompressedDiffInfo(out_info, diff)) {
    *is_window = hpatch_FALSE;
    return hpatch_TRUE;
  }
  if (getWindowDiffInfo(out_win, diff, 0)) {
    *is_window = hpatch_TRUE;
    return hpatch_TRUE;
  }
  return hpatch_FALSE;
}

// winpatch listener that supplies the window-decompress plugin, the xxh128
// checksum plugin and a scratch cache; frees the cache on finish.
struct WinPatchContext {
  std::vector<unsigned char> cache;
};

// Read callback for a zero-length old stream (new-file patch): any non-empty
// read is out of range, so only a no-op read can succeed.
hpatch_BOOL EmptyMemRead(const hpatch_TStreamInput* s,
                         hpatch_StreamPos_t from,
                         unsigned char* out,
                         unsigned char* out_end) {
  (void)s;
  (void)from;
  (void)out;
  return out_end == out;
}

// Bounded in-memory read: count is checked against the remaining size using
// subtraction to avoid uint64 wraparound on hostile patch headers.
hpatch_BOOL CheckedMemRead(const hpatch_TStreamInput* s,
                           hpatch_StreamPos_t from,
                           unsigned char* out,
                           unsigned char* out_end) {
  const unsigned char* base =
      static_cast<const unsigned char*>(s->streamImport);
  hpatch_StreamPos_t count = (hpatch_StreamPos_t)(out_end - out);
  if (count > s->streamSize || from > s->streamSize - count)
    return hpatch_FALSE;
  std::memcpy(out, base + from, (size_t)count);
  return hpatch_TRUE;
}

hpatch_BOOL WinOnDiffInfo(struct winpatch_listener_t* listener,
                          const hpatch_windowDiffInfo* info,
                          hpatch_TDecompress** out_dec,
                          hpatch_TChecksum** out_chk,
                          hpatch_BOOL* isNew,
                          hpatch_BOOL* isOld,
                          hpatch_BOOL* isDiff,
                          unsigned char** out_cache,
                          unsigned char** out_cacheEnd) {
  WinPatchContext* ctx = static_cast<WinPatchContext*>(listener->import);
  size_t cache_size = (size_t)(info->maxWindowOldSize + info->maxStepMemSize) +
                      hpatch_kStreamCacheSize * 3;
  try {
    ctx->cache.resize(cache_size);
  } catch (const std::exception&) {
    // Never let a C++ exception escape into the C patch engine.
    return hpatch_FALSE;
  }
  *out_cache = ctx->cache.data();
  *out_cacheEnd = ctx->cache.data() + ctx->cache.size();
  *out_dec = &zstdDecompressPlugin;
  // Only verify the checksum when it matches a plugin we carry; otherwise
  // disable verification so an unknown checksum type does not fail the patch.
  if (info->checksumType[0] != 0 &&
      std::strcmp(info->checksumType, "xxh128") == 0) {
    *out_chk = &xxh128ChecksumPlugin;
    *isNew = hpatch_TRUE;
  } else {
    *out_chk = nullptr;
    *isNew = hpatch_FALSE;
  }
  *isOld = hpatch_FALSE;
  *isDiff = hpatch_FALSE;
  return hpatch_TRUE;
}

void WinOnPatchFinish(struct winpatch_listener_t* listener,
                      unsigned char* cache,
                      unsigned char* cacheEnd) {
  // cache is owned by WinPatchContext; nothing to free here.
}

}  // namespace

std::string get_hpatchz_path() {
  // Patching is in-process now; no external hpatchz executable is needed.
  // Kept for API compatibility with older consumers.
  return "Quaton";
}

HPatchResult apply_patch(const std::string* source_file,
                         const std::string& patch_file,
                         const std::string& output_file,
                         bool is_compressed) {
  HPatchResult result;
  (void)is_compressed;  // HDiffPatch auto-detects compression from the head.

  try {
    if (!fs::exists(patch_file)) {
      result.error_message = "Patch file does not exist: " + patch_file;
      LOG_ERROR("%s", result.error_message.c_str());
      return result;
    }
    if (source_file && !source_file->empty() && !fs::exists(*source_file)) {
      result.error_message = "Source file does not exist: " + *source_file;
      LOG_ERROR("%s", result.error_message.c_str());
      return result;
    }

    hpatch_TFileStreamInput oldStream;
    hpatch_TFileStreamInput diffStream;
    hpatch_TFileStreamOutput outStream;
    hpatch_TFileStreamInput_init(&oldStream);
    hpatch_TFileStreamInput_init(&diffStream);
    hpatch_TFileStreamOutput_init(&outStream);

    if (!hpatch_TFileStreamInput_open(&diffStream, patch_file.c_str())) {
      result.error_message = "Failed to open patch file: " + patch_file;
      LOG_ERROR("%s", result.error_message.c_str());
      return result;
    }

    // A new-file patch has no source; use an empty (0-byte) old stream with a
    // valid read callback, as the patch engine dereferences it unconditionally.
    const hpatch_TStreamInput* oldPtr = nullptr;
    if (source_file && !source_file->empty()) {
      if (!hpatch_TFileStreamInput_open(&oldStream, source_file->c_str())) {
        result.error_message = "Failed to open source file: " + *source_file;
        LOG_ERROR("%s", result.error_message.c_str());
        hpatch_TFileStreamInput_close(&diffStream);
        return result;
      }
      oldPtr = &oldStream.base;
    } else {
      oldStream.base.streamImport = nullptr;
      oldStream.base.streamSize = 0;
      oldStream.base.read = EmptyMemRead;
      oldPtr = &oldStream.base;
    }

    hpatch_compressedDiffInfo diffInfo;
    hpatch_windowDiffInfo winDiff;
    hpatch_BOOL is_window = hpatch_FALSE;
    if (!GetDiffInfo(&diffInfo, &winDiff, &is_window, &diffStream.base)) {
      result.error_message = "Unsupported or corrupt patch format";
      LOG_ERROR("%s", result.error_message.c_str());
      hpatch_TFileStreamInput_close(&oldStream);
      hpatch_TFileStreamInput_close(&diffStream);
      return result;
    }
    const hpatch_StreamPos_t oldDataSize =
        is_window ? winDiff.oldDataSize : diffInfo.oldDataSize;
    if (oldDataSize != oldPtr->streamSize) {
      result.error_message = "Source size does not match patch expectation";
      LOG_ERROR("%s", result.error_message.c_str());
      hpatch_TFileStreamInput_close(&oldStream);
      hpatch_TFileStreamInput_close(&diffStream);
      return result;
    }

    const hpatch_StreamPos_t newDataSize =
        is_window ? winDiff.newDataSize : diffInfo.newDataSize;
    if (!hpatch_TFileStreamOutput_open(
            &outStream, output_file.c_str(), newDataSize)) {
      result.error_message = "Failed to open output file: " + output_file;
      LOG_ERROR("%s", result.error_message.c_str());
      hpatch_TFileStreamInput_close(&oldStream);
      hpatch_TFileStreamInput_close(&diffStream);
      return result;
    }

    bool ok = false;
    if (is_window) {
      WinPatchContext ctx;
      struct winpatch_listener_t listener;
      std::memset(&listener, 0, sizeof(listener));
      listener.import = &ctx;
      listener.onDiffInfo = WinOnDiffInfo;
      listener.onPatchFinish = WinOnPatchFinish;
      TWindowPatchResult wr = patch_window_diff(
          &listener, &outStream.base, oldPtr, &diffStream.base, 0, 1);
      ok = (wr == kWindowPatch_ok);
      if (!ok) {
        result.error_message =
            "Window patch failed, result=" + std::to_string((int)wr);
        LOG_ERROR("%s", result.error_message.c_str());
      }
    } else {
      std::vector<unsigned char> cache(hpatch_kStreamCacheSize * 16 + 2048);
      ok = patch_decompress_with_cache(&outStream.base,
                                       oldPtr,
                                       &diffStream.base,
                                       &zstdDecompressPlugin,
                                       cache.data(),
                                       cache.data() + cache.size());
      if (!ok) {
        result.error_message = "Compressed patch failed";
        LOG_ERROR("%s", result.error_message.c_str());
      }
    }

    hpatch_TFileStreamOutput_flush(&outStream);
    hpatch_TFileStreamOutput_close(&outStream);

    result.exit_code = ok ? 0 : 1;
    result.success = ok;
    if (!ok && result.error_message.empty()) {
      result.error_message = "Patch application failed";
    }
    if (!ok) {
      // Remove the truncated output left behind by a failed patch.
      fs::remove(output_file);
    }

    hpatch_TFileStreamInput_close(&oldStream);
    hpatch_TFileStreamInput_close(&diffStream);
    return result;

  } catch (const std::exception& e) {
    result.error_message = std::string("Exception in apply_patch: ") + e.what();
    LOG_ERROR("%s", result.error_message.c_str());
    return result;
  }
}

HPatchResult apply_patch_from_memory(const std::string* source_file,
                                     const uint8_t* patch_data,
                                     size_t patch_size,
                                     const std::string& output_file,
                                     bool is_compressed) {
  HPatchResult result;
  (void)is_compressed;

  try {
    if (!patch_data || patch_size == 0) {
      result.error_message = "Invalid patch data";
      LOG_ERROR("%s", result.error_message.c_str());
      return result;
    }
    if (source_file && !source_file->empty() && !fs::exists(*source_file)) {
      result.error_message = "Source file does not exist: " + *source_file;
      LOG_ERROR("%s", result.error_message.c_str());
      return result;
    }

    // Wrap the in-memory patch as a random-read input stream.
    hpatch_TStreamInput diffStream;
    std::memset(&diffStream, 0, sizeof(diffStream));
    diffStream.streamImport = const_cast<unsigned char*>(patch_data);
    diffStream.streamSize = (hpatch_StreamPos_t)patch_size;
    diffStream.read = CheckedMemRead;

    hpatch_TFileStreamInput oldStream;
    hpatch_TFileStreamInput_init(&oldStream);
    const hpatch_TStreamInput* oldPtr = nullptr;
    hpatch_TStreamInput emptyOld;
    if (source_file && !source_file->empty()) {
      if (!hpatch_TFileStreamInput_open(&oldStream, source_file->c_str())) {
        result.error_message = "Failed to open source file: " + *source_file;
        LOG_ERROR("%s", result.error_message.c_str());
        return result;
      }
      oldPtr = &oldStream.base;
    } else {
      // Zero-length old stream with a valid read callback (new-file patch).
      std::memset(&emptyOld, 0, sizeof(emptyOld));
      emptyOld.read = EmptyMemRead;
      oldPtr = &emptyOld;
    }

    hpatch_compressedDiffInfo diffInfo;
    hpatch_windowDiffInfo winDiff;
    hpatch_BOOL is_window = hpatch_FALSE;
    if (!GetDiffInfo(&diffInfo, &winDiff, &is_window, &diffStream)) {
      result.error_message = "Unsupported or corrupt patch format";
      LOG_ERROR("%s", result.error_message.c_str());
      hpatch_TFileStreamInput_close(&oldStream);
      return result;
    }
    const hpatch_StreamPos_t oldDataSize =
        is_window ? winDiff.oldDataSize : diffInfo.oldDataSize;
    if (oldDataSize != oldPtr->streamSize) {
      result.error_message = "Source size does not match patch expectation";
      LOG_ERROR("%s", result.error_message.c_str());
      hpatch_TFileStreamInput_close(&oldStream);
      return result;
    }

    const hpatch_StreamPos_t newDataSize =
        is_window ? winDiff.newDataSize : diffInfo.newDataSize;
    hpatch_TFileStreamOutput outStream;
    hpatch_TFileStreamOutput_init(&outStream);
    if (!hpatch_TFileStreamOutput_open(
            &outStream, output_file.c_str(), newDataSize)) {
      result.error_message = "Failed to open output file: " + output_file;
      LOG_ERROR("%s", result.error_message.c_str());
      hpatch_TFileStreamInput_close(&oldStream);
      return result;
    }

    bool ok = false;
    if (is_window) {
      WinPatchContext ctx;
      struct winpatch_listener_t listener;
      std::memset(&listener, 0, sizeof(listener));
      listener.import = &ctx;
      listener.onDiffInfo = WinOnDiffInfo;
      listener.onPatchFinish = WinOnPatchFinish;
      TWindowPatchResult wr = patch_window_diff(
          &listener, &outStream.base, oldPtr, &diffStream, 0, 1);
      ok = (wr == kWindowPatch_ok);
      if (!ok) {
        result.error_message =
            "Window patch failed, result=" + std::to_string((int)wr);
        LOG_ERROR("%s", result.error_message.c_str());
      }
    } else {
      std::vector<unsigned char> cache(hpatch_kStreamCacheSize * 16 + 2048);
      ok = patch_decompress_with_cache(&outStream.base,
                                       oldPtr,
                                       &diffStream,
                                       &zstdDecompressPlugin,
                                       cache.data(),
                                       cache.data() + cache.size());
      if (!ok) {
        result.error_message = "Compressed patch failed";
        LOG_ERROR("%s", result.error_message.c_str());
      }
    }

    hpatch_TFileStreamOutput_flush(&outStream);
    hpatch_TFileStreamOutput_close(&outStream);

    result.exit_code = ok ? 0 : 1;
    result.success = ok;
    if (!ok && result.error_message.empty()) {
      result.error_message = "Patch application failed";
    }
    if (!ok) {
      // Remove the truncated output left behind by a failed patch.
      fs::remove(output_file);
    }

    hpatch_TFileStreamInput_close(&oldStream);
    return result;

  } catch (const std::exception& e) {
    result.error_message =
        std::string("Exception in apply_patch_from_memory: ") + e.what();
    LOG_ERROR("%s", result.error_message.c_str());
    return result;
  }
}

bool is_hpatchz_available() {
  // Patching is now in-process; the external tool is no longer required.
  return true;
}

std::string get_hpatchz_version() {
  return std::to_string(HDIFFPATCH_VERSION_MAJOR) + "." +
         std::to_string(HDIFFPATCH_VERSION_MINOR) + "." +
         std::to_string(HDIFFPATCH_VERSION_RELEASE);
}

}  // namespace HPatchUtils
}  // namespace Quaton
