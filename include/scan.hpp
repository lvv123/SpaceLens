#pragma once

// Scanning entry points. `scan_volume_mft` is the fast path (admin + NTFS);
// `scan_directory_recursive` is the portable path used for tests and for
// non-elevated development.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "tree.hpp"

namespace cs {

struct ScanProgress {
    std::atomic<uint64_t> recordsRead{0};
    std::atomic<uint64_t> totalRecords{0};
    std::atomic<uint64_t> bytesRead{0};
    std::atomic<uint64_t> fileCount{0};
    std::atomic<uint64_t> dirCount{0};
    std::atomic<uint64_t> orphanNodes{0};   // nodes created but not linked to the root
    std::atomic<uint64_t> orphanBytes{0};   // their summed self size
    std::atomic<uint64_t> orphanNoParent{0};   // unlinked: parent record/dir not found
    std::atomic<uint64_t> orphanSeqMismatch{0};  // unlinked: parent sequence mismatch
    std::atomic<uint64_t> orphanParentOutOfRange{0};
    std::atomic<uint64_t> orphanParentNotInUse{0};
    std::atomic<uint64_t> orphanParentNotDir{0};
    std::atomic<uint64_t> orphanParentNoNode{0};
    std::atomic<uint64_t> recordsNotInUse{0};
    std::atomic<uint64_t> recordsMetadata{0};
    std::atomic<uint64_t> recordsNoName{0};
    std::atomic<uint64_t> recordsFixupFail{0};
    std::atomic<uint64_t> recordsParseFail{0};
    std::atomic<uint64_t> sizeFollowAttempts{0};
    std::atomic<uint64_t> sizeFollowSuccess{0};
    std::atomic<bool>     cancel{false};
    std::atomic<bool>     finished{false};
    std::atomic<bool>     failed{false};

    // 中文注释：一次扫描开始前把全部计数和状态清零。逐个字段重置比赋值整个
    // 结构体安全，因为 std::atomic 不可拷贝；cancel 也由调用方在需要时预先设置。
    void reset() {
        recordsRead.store(0, std::memory_order_relaxed);
        totalRecords.store(0, std::memory_order_relaxed);
        bytesRead.store(0, std::memory_order_relaxed);
        fileCount.store(0, std::memory_order_relaxed);
        dirCount.store(0, std::memory_order_relaxed);
        orphanNodes.store(0, std::memory_order_relaxed);
        orphanBytes.store(0, std::memory_order_relaxed);
        orphanNoParent.store(0, std::memory_order_relaxed);
        orphanSeqMismatch.store(0, std::memory_order_relaxed);
        orphanParentOutOfRange.store(0, std::memory_order_relaxed);
        orphanParentNotInUse.store(0, std::memory_order_relaxed);
        orphanParentNotDir.store(0, std::memory_order_relaxed);
        orphanParentNoNode.store(0, std::memory_order_relaxed);
        recordsNotInUse.store(0, std::memory_order_relaxed);
        recordsMetadata.store(0, std::memory_order_relaxed);
        recordsNoName.store(0, std::memory_order_relaxed);
        recordsFixupFail.store(0, std::memory_order_relaxed);
        recordsParseFail.store(0, std::memory_order_relaxed);
        sizeFollowAttempts.store(0, std::memory_order_relaxed);
        sizeFollowSuccess.store(0, std::memory_order_relaxed);
        cancel.store(false, std::memory_order_relaxed);
        finished.store(false, std::memory_order_relaxed);
        failed.store(false, std::memory_order_relaxed);
    }
};

// Drive roots of local NTFS volumes, e.g. {"C:", "D:"}.
std::vector<std::wstring> list_ntfs_volumes();

// Source of raw volume bytes. Real scans use a volume handle; tests use an
// in-memory image.
struct VolumeReader {
    virtual ~VolumeReader() = default;
    virtual bool read(uint64_t offset, void* buffer, uint32_t size) = 0;
};

// Scan an NTFS volume image through `reader` by parsing $MFT directly.
// This is the testable core of scan_volume_mft.
bool scan_mft(VolumeReader& reader, const std::wstring& volumeRoot, VolumeTree& tree,
              ScanProgress& progress, std::wstring& error);

// Scan a whole NTFS volume by parsing $MFT directly. Requires an elevated
// process. Returns false and sets `error` on failure.
bool scan_volume_mft(const std::wstring& volumeRoot, VolumeTree& tree,
                     ScanProgress& progress, std::wstring& error);

// Recursively scan a directory with FindFirstFileW (no privileges needed).
bool scan_directory_recursive(const std::wstring& path, VolumeTree& tree,
                              ScanProgress& progress, std::wstring& error);

// Format a Win32 error code as a readable message.
std::wstring format_win32_error(uint32_t code);

}  // namespace cs
