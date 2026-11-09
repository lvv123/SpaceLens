#include "scan.hpp"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <new>

#include "ntfs.hpp"

namespace cs {
namespace {

struct HandleGuard {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~HandleGuard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    HandleGuard() = default;
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
};

// Reads raw volume bytes through a real (synchronous) volume handle.
struct HandleReader : VolumeReader {
    HANDLE h = INVALID_HANDLE_VALUE;

    bool read(uint64_t offset, void* buf, uint32_t size) override {
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
        DWORD total = 0;
        auto* p = static_cast<uint8_t*>(buf);
        while (total < size) {
            DWORD got = 0;
            if (!ReadFile(h, p + total, size - total, &got, nullptr)) return false;
            if (got == 0) return false;
            total += got;
        }
        return true;
    }
};

}  // namespace

std::wstring format_win32_error(uint32_t code) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring s;
    if (n != 0 && buf != nullptr) {
        s.assign(buf, n);
        while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' '))
            s.pop_back();
    } else {
        s = L"错误码 " + std::to_wstring(code);
    }
    if (buf != nullptr) LocalFree(buf);
    return s;
}

std::vector<std::wstring> list_ntfs_volumes() {
    std::vector<std::wstring> out;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        const wchar_t letter = static_cast<wchar_t>(L'A' + i);
        wchar_t root[4] = {letter, L':', L'\\', 0};
        const UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        wchar_t fsName[MAX_PATH + 1] = {};
        if (GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fsName,
                                  MAX_PATH + 1) == FALSE)
            continue;
        if (wcscmp(fsName, L"NTFS") == 0)
            out.push_back(std::wstring(1, letter) + L":");
    }
    return out;
}

static bool scan_mft_impl(VolumeReader& reader, const std::wstring& volumeRoot,
                          VolumeTree& tree, ScanProgress& progress, std::wstring& error) {
    // 中文注释：调用方负责在启动前用 ScanProgress::reset() 清零进度。这里
    // 不能再次 reset，否则会把用户刚设置的 cancel 标志覆盖掉。
    if (progress.cancel.load(std::memory_order_relaxed)) {
        tree.clear();
        error = L"扫描已取消";
        progress.finished.store(false, std::memory_order_relaxed);
        progress.failed.store(true, std::memory_order_relaxed);
        return false;
    }

    tree.clear();
    error.clear();

    uint8_t boot[4096];
    if (!reader.read(0, boot, sizeof(boot))) {
        error = L"读取引导扇区失败";
        progress.failed = true;
        return false;
    }
    ntfs::BootSector bs;
    if (!ntfs::parse_boot_sector(boot, sizeof(boot), bs)) {
        error = L"不是有效的 NTFS 卷（引导扇区校验失败）";
        progress.failed = true;
        return false;
    }

    const uint32_t recSize = bs.bytesPerFileRecord;
    if (recSize < 64 || recSize > 4096) {
        error = L"异常的 MFT 记录大小";
        progress.failed = true;
        return false;
    }

    // Read the $MFT record itself to learn where the MFT lives (run list).
    const uint32_t align = std::max<uint32_t>(bs.bytesPerSector, 512);
    const uint32_t readLen = ((recSize + align - 1) / align) * align;
    std::vector<uint8_t> mftRec(readLen, 0);
    const uint64_t mftOffset = static_cast<uint64_t>(bs.mftLcn) * bs.bytesPerCluster;
    if (!reader.read(mftOffset, mftRec.data(), readLen)) {
        error = L"读取 $MFT 记录失败";
        progress.failed = true;
        return false;
    }
    if (!ntfs::apply_fixups(mftRec.data(), recSize, bs.bytesPerSector)) {
        error = L"$MFT 记录校验失败";
        progress.failed = true;
        return false;
    }
    std::vector<ntfs::Run> runs;
    uint64_t mftReal = 0;
    uint64_t mftAlloc = 0;
    if (!ntfs::decode_data_attribute(mftRec.data(), recSize, runs, mftReal, mftAlloc)) {
        error = L"无法解析 $MFT 数据分布表";
        progress.failed = true;
        return false;
    }

    const uint64_t recordCount = mftReal / recSize;
    progress.totalRecords.store(recordCount, std::memory_order_relaxed);
    if (recordCount == 0) {
        error = L"MFT 为空";
        progress.failed = true;
        return false;
    }
    // 中文注释：目录索引、记录状态等数组按 uint32 记录号索引；超过 2^32-1
    // 的记录数无法表示，也可能导致后续 size_t 乘法异常，必须提前拒绝。
    if (recordCount > 0xFFFFFFFFull) {
        error = L"MFT 记录数量超出支持范围";
        progress.failed = true;
        return false;
    }

    tree.reserve(static_cast<size_t>(recordCount), static_cast<size_t>(recordCount) * 16u);

    std::vector<uint32_t> dirNode(static_cast<size_t>(recordCount), VolumeTree::kInvalid);
    std::vector<uint16_t> recSeq(static_cast<size_t>(recordCount), 0);
    std::vector<uint64_t> parentRef;
    parentRef.reserve(static_cast<size_t>(recordCount));

    uint64_t rootNode = VolumeTree::kInvalid;
    uint32_t recIndex = 0;
    uint64_t recordsProcessed = 0;
    uint64_t sizeFollowAttempts = 0;
    uint64_t sizeFollowSuccess = 0;
    bool cancelled = false;

    ntfs::FileRecord fr;
    ntfs::FileRecord frExt;
    std::vector<uint8_t> attrListBuf;

    // Map an MFT record number to its bytes via the $MFT run list (used to
    // follow $ATTRIBUTE_LIST references into extension records).
    auto read_record = [&](uint64_t recordNumber, uint8_t* buf) -> bool {
        const uint64_t streamOffset = recordNumber * recSize;
        for (const auto& run : runs) {
            const uint64_t runStart = run.vcn * bs.bytesPerCluster;
            const uint64_t runBytes = run.lengthClusters * bs.bytesPerCluster;
            if (streamOffset < runStart || streamOffset >= runStart + runBytes) continue;
            if (run.sparse) return false;
            const uint64_t phys = run.lcn * bs.bytesPerCluster + (streamOffset - runStart);
            return reader.read(phys, buf, recSize);
        }
        return false;
    };

    // Read up to `maxBytes` of a stream described by `streamRuns` into `out`.
    auto read_stream = [&](const std::vector<ntfs::Run>& streamRuns, uint64_t maxBytes,
                           std::vector<uint8_t>& out) -> bool {
        out.clear();
        for (const auto& run : streamRuns) {
            const uint64_t runBytes = run.lengthClusters * bs.bytesPerCluster;
            uint64_t off = run.lcn * bs.bytesPerCluster;
            uint64_t left = runBytes;
            while (left > 0 && out.size() < maxBytes) {
                const size_t n =
                    static_cast<size_t>(std::min<uint64_t>(left, maxBytes - out.size()));
                const size_t old = out.size();
                out.resize(old + n);
                if (run.sparse) {
                    std::memset(out.data() + old, 0, n);
                } else if (!reader.read(off, out.data() + old, static_cast<uint32_t>(n))) {
                    return false;
                }
                off += n;
                left -= n;
            }
            if (out.size() >= maxBytes) break;
        }
        return true;
    };

    std::vector<uint8_t> recStatus(static_cast<size_t>(recordCount), 0);  // 1=inUse 2=dir 4=hasNode
    uint64_t cNotInUse = 0, cMetadata = 0, cNoName = 0, cFixupFail = 0, cParseFail = 0;

    auto handleRecord = [&](uint8_t* rec) {
        const uint32_t idx = recIndex++;
        ++recordsProcessed;
        progress.recordsRead.store(recordsProcessed, std::memory_order_relaxed);
        if (progress.cancel.load(std::memory_order_relaxed)) {
            cancelled = true;
            return;
        }
        if (!ntfs::apply_fixups(rec, recSize, bs.bytesPerSector)) { ++cFixupFail; return; }
        if (!ntfs::parse_file_record(rec, recSize, idx, bs.bytesPerSector, fr)) { ++cParseFail; return; }
        if (!fr.inUse) { ++cNotInUse; return; }
        if (idx < 16 && idx != 5) { ++cMetadata; return; }  // NTFS metadata records

        recStatus[idx] |= 0x01;
        if (fr.isDirectory) recStatus[idx] |= 0x02;

        if (idx == 5) {
            rootNode = tree.addNode(volumeRoot.c_str(),
                                    static_cast<uint32_t>(volumeRoot.size()), 0, 0, true, idx);
            parentRef.push_back(0);
            dirNode[5] = static_cast<uint32_t>(rootNode);
            recSeq[5] = fr.sequence;
            recStatus[5] |= 0x04;
            return;
        }

        uint64_t selfSize = fr.isDirectory ? 0 : fr.realSize;
        uint64_t selfAlloc = fr.isDirectory ? 0 : fr.allocatedSize;

        // Large/fragmented files keep their $DATA in an extension record listed
        // by $ATTRIBUTE_LIST; the base record's size fields are 0. Follow it.
        if (!fr.isDirectory && selfSize == 0) {
            uint64_t dataRef = 0;
            bool haveRef = fr.dataExtentRefValid;
            if (haveRef) {
                dataRef = fr.dataExtentRef;
            } else if (fr.hasAttrListRuns) {
                const uint64_t want = std::min<uint64_t>(fr.attrListRealSize, 256u * 1024u);
                if (read_stream(fr.attrListRuns, want, attrListBuf) && !attrListBuf.empty()) {
                    uint64_t ref = 0;
                    if (ntfs::find_data_extent_in_attribute_list(
                            attrListBuf.data(), static_cast<uint32_t>(attrListBuf.size()), ref)) {
                        dataRef = ref;
                        haveRef = true;
                    }
                }
            }
            if (haveRef) {
                const uint64_t refRec = dataRef & 0xFFFFFFFFFFFFull;
                if (refRec != idx && refRec < recordCount) {
                    ++sizeFollowAttempts;
                    uint8_t extBuf[4096];
                    if (read_record(refRec, extBuf) &&
                        ntfs::apply_fixups(extBuf, recSize, bs.bytesPerSector) &&
                        ntfs::parse_file_record(extBuf, recSize, static_cast<uint32_t>(refRec),
                                                bs.bytesPerSector, frExt) &&
                        frExt.realSize != 0) {
                        selfSize = frExt.realSize;
                        selfAlloc = frExt.allocatedSize;
                        ++sizeFollowSuccess;
                    }
                }
            }
        }

        // Pick one name per parent directory (hard links -> several parents),
        // preferring Win32 namespaces but falling back to POSIX/DOS names so
        // entries created by POSIX-semantics tooling are not dropped.
        // Pick one name per parent *record* (hard links -> several parents).
        // Names for the same parent may differ only in the sequence bits, so key
        // on the record number, not the full reference.
        uint64_t chosenKey[32];
        size_t chosenIdx[32];
        uint8_t chosenRank[32];
        size_t chosenCount = 0;
        for (size_t a = 0; a < fr.names.size(); ++a) {
            const auto& e = fr.names[a];
            if (e.nameLength == 0) continue;
            const uint64_t key = e.parentRef & 0xFFFFFFFFFFFFull;
            const uint8_t rank = (e.nameSpace == 1 || e.nameSpace == 3) ? 0 : 1;
            size_t slot = chosenCount;
            for (size_t k = 0; k < chosenCount; ++k) {
                if (chosenKey[k] == key) { slot = k; break; }
            }
            if (slot == chosenCount) {
                if (chosenCount >= 32) continue;  // pathological number of names
                chosenKey[chosenCount] = key;
                chosenIdx[chosenCount] = a;
                chosenRank[chosenCount] = rank;
                ++chosenCount;
            } else if (rank < chosenRank[slot]) {
                chosenIdx[slot] = a;
                chosenRank[slot] = rank;
            }
        }

        bool created = false;
        for (size_t k = 0; k < chosenCount; ++k) {
            const auto& e = fr.names[chosenIdx[k]];
            const uint32_t node = tree.addNode(e.name, e.nameLength, selfSize, selfAlloc,
                                               fr.isDirectory, idx);
            parentRef.push_back(e.parentRef);
            created = true;
            if (fr.isDirectory && dirNode[idx] == VolumeTree::kInvalid) {
                dirNode[idx] = node;
                recSeq[idx] = fr.sequence;
            }
        }
        if (created) {
            recStatus[idx] |= 0x04;
        } else {
            ++cNoName;
        }
    };

    uint64_t chunkBytes = 8ull << 20;
    if (chunkBytes < bs.bytesPerCluster) {
        chunkBytes = bs.bytesPerCluster;
    } else {
        chunkBytes -= chunkBytes % bs.bytesPerCluster;
    }
    std::vector<uint8_t> chunk(static_cast<size_t>(chunkBytes));
    std::vector<uint8_t> carry;
    carry.reserve(recSize * 4);
    size_t carryPos = 0;

    auto feed = [&](const uint8_t* data, size_t n) {
        if (carryPos > 0) {
            carry.erase(carry.begin(), carry.begin() + static_cast<std::ptrdiff_t>(carryPos));
            carryPos = 0;
        }
        carry.insert(carry.end(), data, data + n);
        size_t i = 0;
        while (recIndex < recordCount && carry.size() - i >= recSize && !cancelled) {
            handleRecord(carry.data() + i);
            i += recSize;
        }
        carryPos = i;
    };

    bool readFailed = false;
    for (const auto& run : runs) {
        if (cancelled || readFailed || recIndex >= recordCount) break;
        const uint64_t runBytes = run.lengthClusters * bs.bytesPerCluster;
        uint64_t off = run.lcn * bs.bytesPerCluster;
        uint64_t left = runBytes;
        while (left > 0 && !cancelled && recIndex < recordCount) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(left, chunkBytes));
            if (run.sparse) {
                std::memset(chunk.data(), 0, n);
            } else if (!reader.read(off, chunk.data(), static_cast<uint32_t>(n))) {
                readFailed = true;
                break;
            } else {
                progress.bytesRead.fetch_add(n, std::memory_order_relaxed);
            }
            feed(chunk.data(), n);
            off += n;
            left -= n;
        }
    }

    // 中文注释：取消时丢弃已建立的半成品树，并明确报告失败；不能把取消
    // 当成一次成功扫描发布给 UI。
    if (cancelled || progress.cancel.load(std::memory_order_relaxed)) {
        tree.clear();
        error = L"扫描已取消";
        progress.finished.store(false, std::memory_order_relaxed);
        progress.failed.store(true, std::memory_order_relaxed);
        return false;
    }
    if (readFailed) {
        error = L"读取 MFT 数据时失败";
        progress.failed = true;
        return false;
    }
    if (rootNode == VolumeTree::kInvalid) {
        error = L"未能定位卷根目录记录";
        progress.failed = true;
        return false;
    }

    // Link children to parents, validating the sequence number so recycled
    // records cannot be attached to the wrong directory.
    uint64_t orphanNoParent = 0;
    uint64_t orphanSeqMismatch = 0;
    uint64_t opOutOfRange = 0, opNotInUse = 0, opNotDir = 0, opNoNode = 0;
    for (uint32_t i = 0; i < tree.nodeCount(); ++i) {
        if (i == rootNode) continue;
        const uint64_t pref = parentRef[i];
        const uint32_t prec = static_cast<uint32_t>(pref & 0xFFFFFFFFFFFFull);
        const uint16_t pseq = static_cast<uint16_t>(pref >> 48);
        if (prec >= recordCount || prec == i) {
            ++orphanNoParent;
            ++opOutOfRange;
            continue;
        }
        const uint8_t st = recStatus[prec];
        if ((st & 0x01) == 0) { ++orphanNoParent; ++opNotInUse; continue; }
        if ((st & 0x02) == 0) { ++orphanNoParent; ++opNotDir; continue; }
        const uint32_t pnode = dirNode[prec];
        if (pnode == VolumeTree::kInvalid) { ++orphanNoParent; ++opNoNode; continue; }
        if (recSeq[prec] != pseq) {
            ++orphanSeqMismatch;
            continue;
        }
        tree.link(pnode, i);
    }
    progress.orphanNoParent.store(orphanNoParent, std::memory_order_relaxed);
    progress.orphanSeqMismatch.store(orphanSeqMismatch, std::memory_order_relaxed);
    progress.orphanParentOutOfRange.store(opOutOfRange, std::memory_order_relaxed);
    progress.orphanParentNotInUse.store(opNotInUse, std::memory_order_relaxed);
    progress.orphanParentNotDir.store(opNotDir, std::memory_order_relaxed);
    progress.orphanParentNoNode.store(opNoNode, std::memory_order_relaxed);
    progress.recordsNotInUse.store(cNotInUse, std::memory_order_relaxed);
    progress.recordsMetadata.store(cMetadata, std::memory_order_relaxed);
    progress.recordsNoName.store(cNoName, std::memory_order_relaxed);
    progress.recordsFixupFail.store(cFixupFail, std::memory_order_relaxed);
    progress.recordsParseFail.store(cParseFail, std::memory_order_relaxed);
    progress.sizeFollowAttempts.store(sizeFollowAttempts, std::memory_order_relaxed);
    progress.sizeFollowSuccess.store(sizeFollowSuccess, std::memory_order_relaxed);

    // Diagnostics: how many nodes exist and how many could not be linked.
    // (totalSize still holds each node's own size here; aggregate() runs next.)
    {
        uint64_t fileCount = 0, dirCount = 0, orphans = 0, orphanBytes = 0;
        for (uint32_t i = 0; i < tree.nodeCount(); ++i) {
            const auto& n = tree.node(i);
            if (tree.isDirectory(i)) ++dirCount; else ++fileCount;
            if (i != rootNode && n.parent == VolumeTree::kInvalid) {
                ++orphans;
                orphanBytes += n.totalSize;
            }
        }
        progress.fileCount.store(fileCount, std::memory_order_relaxed);
        progress.dirCount.store(dirCount, std::memory_order_relaxed);
        progress.orphanNodes.store(orphans, std::memory_order_relaxed);
        progress.orphanBytes.store(orphanBytes, std::memory_order_relaxed);
    }

    tree.setRoot(static_cast<uint32_t>(rootNode));
    tree.aggregate();
    tree.sortChildrenBySizeDesc();
    progress.finished.store(true, std::memory_order_relaxed);
    return true;
}

bool scan_mft(VolumeReader& reader, const std::wstring& volumeRoot, VolumeTree& tree,
              ScanProgress& progress, std::wstring& error) {
    // 中文注释：磁盘数据或容量异常可能触发内存分配失败；在扫描入口统一
    // 转换成错误结果，避免异常从 GUI 工作线程逃逸导致 std::terminate。
    try {
        return scan_mft_impl(reader, volumeRoot, tree, progress, error);
    } catch (const std::bad_alloc&) {
        tree.clear();
        error = L"内存不足，扫描已中止";
        progress.finished.store(false, std::memory_order_relaxed);
        progress.failed.store(true, std::memory_order_relaxed);
        return false;
    } catch (const std::exception&) {
        tree.clear();
        error = L"扫描时发生内部异常";
        progress.finished.store(false, std::memory_order_relaxed);
        progress.failed.store(true, std::memory_order_relaxed);
        return false;
    }
}

bool scan_volume_mft(const std::wstring& volumeRoot, VolumeTree& tree,
                     ScanProgress& progress, std::wstring& error) {
    const std::wstring device = L"\\\\.\\" + volumeRoot;
    HandleGuard guard;
    guard.h = CreateFileW(device.c_str(), GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                          FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (guard.h == INVALID_HANDLE_VALUE) {
        tree.clear();
        error = L"无法打开卷 " + volumeRoot + L"（需要管理员权限，且必须是 NTFS）: " +
                format_win32_error(GetLastError());
        progress.failed = true;
        return false;
    }
    HandleReader reader;
    reader.h = guard.h;
    return scan_mft(reader, volumeRoot, tree, progress, error);
}

}  // namespace cs
