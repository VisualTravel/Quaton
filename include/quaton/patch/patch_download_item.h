#ifndef QUATON_PATCH_PATCH_DOWNLOAD_ITEM_H_
#define QUATON_PATCH_PATCH_DOWNLOAD_ITEM_H_

#include <memory>
#include <string>

#include "quaton/base/download_item.h"
#include "quaton/patch.h"
#include "quaton/quaton_global.h"

QUATON_NAMESPACE_BEGIN

/**
 * @enum PatchType
 * @brief Type of patch format
 */
enum class PatchType {
  kLdiff,   // LDIFF format
  kHPatch,  // HPatch format
  kBsDiff,  // BsDiff format
  kXDelta,  // XDelta format
  kUnknown
};

/**
 * @class PatchDownloadItem
 * @brief Download item for patch-based updates
 *
 * Extends base DownloadItem with patch-specific properties:
 * - Source file path
 * - Target file path
 * - Patch type (LDIFF/HPatch)
 * - Apply status
 */
class QUATON_API PatchDownloadItem : public DownloadItem {
 public:
  /**
   * @brief Constructor
   * @param patch_name Patch file name
   * @param patch_size Patch file size
   * @param patch_hash Patch file hash
   */
  PatchDownloadItem(const std::string& patch_name,
                    int64_t patch_size,
                    const std::string& patch_hash);

  // ========== Patch Properties ==========

  /**
   * @brief Get/Set patch type
   */
  PatchType GetPatchType() const { return patch_type_; }
  void SetPatchType(PatchType type) { patch_type_ = type; }

  /**
   * @brief Get patch type as string
   */
  std::string GetPatchTypeString() const;

  /**
   * @brief Get/Set source file path (file to be patched)
   */
  const std::string& GetSourceFilePath() const { return source_file_path_; }
  void SetSourceFilePath(const std::string& path) { source_file_path_ = path; }

  /**
   * @brief Get/Set target file path (output after patching)
   */
  const std::string& GetTargetFilePath() const { return target_file_path_; }
  void SetTargetFilePath(const std::string& path) { target_file_path_ = path; }

  /**
   * @brief Get/Set expected target hash (after applying patch)
   */
  const std::string& GetTargetHash() const { return target_hash_; }
  void SetTargetHash(const std::string& hash) { target_hash_ = hash; }

  /**
   * @brief Get/Set expected target size
   */
  int64_t GetTargetSize() const { return target_size_; }
  void SetTargetSize(int64_t size) { target_size_ = size; }

  // ========== Version Info ==========

  /**
   * @brief Get/Set source version
   */
  const std::string& GetSourceVersion() const { return source_version_; }
  void SetSourceVersion(const std::string& ver) { source_version_ = ver; }

  /**
   * @brief Get/Set target version
   */
  const std::string& GetTargetVersion() const { return target_version_; }
  void SetTargetVersion(const std::string& ver) { target_version_ = ver; }

  // ========== LDIFF Specific ==========

  /**
   * @brief Get/Set LDIFF chunk count
   */
  size_t GetLdiffChunkCount() const { return ldiff_chunk_count_; }
  void SetLdiffChunkCount(size_t count) { ldiff_chunk_count_ = count; }

  /**
   * @brief Get/Set LDIFF operation count
   */
  size_t GetLdiffOpCount() const { return ldiff_op_count_; }
  void SetLdiffOpCount(size_t count) { ldiff_op_count_ = count; }

  // ========== Apply Status ==========

  /**
   * @brief Check if patch has been applied
   */
  bool IsApplied() const { return is_applied_; }

  /**
   * @brief Mark patch as applied
   */
  void SetApplied(bool applied) { is_applied_ = applied; }

  /**
   * @brief Get/Set apply progress (0.0 - 1.0)
   */
  double GetApplyProgress() const { return apply_progress_; }
  void SetApplyProgress(double progress) { apply_progress_ = progress; }

  // ========== Convenience Methods ==========

  /**
   * @brief Get file name (alias for GetId)
   */
  const std::string& GetFileName() const { return GetId(); }

  /**
   * @brief Get total size
   */
  int64_t GetTotalSize() const { return GetExpectedSize(); }

  /**
   * @brief Get downloaded size
   */
  int64_t GetDownloadedSize() const { return GetProgress().downloaded_bytes; }

 protected:
  void OnStateChanged(DownloadState old_state,
                      DownloadState new_state) override;

 private:
  PatchType patch_type_ = PatchType::kLdiff;
  std::string source_file_path_;
  std::string target_file_path_;
  std::string target_hash_;
  int64_t target_size_ = 0;
  std::string source_version_;
  std::string target_version_;
  size_t ldiff_chunk_count_ = 0;
  size_t ldiff_op_count_ = 0;
  bool is_applied_ = false;
  double apply_progress_ = 0.0;
};

QUATON_NAMESPACE_END

#endif  // QUATON_PATCH_PATCH_DOWNLOAD_ITEM_H_
