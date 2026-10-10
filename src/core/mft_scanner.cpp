#include "scan.hpp"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

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

bool scan_mft(VolumeReader& reader, const std::wstring& volumeRoot, VolumeTree& tree,
              ScanProgress& progress, std::wstring& error) {
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

    tree.reserve(static_cast<size_t>(recordCount), static_cast<size_t>(recordCount) * 16u);

    std::vector<uint32_t> dirNode(static_cast<size_t>(recordCount), VolumeTree::kInvalid);
    std::vector<uint16_t> recSeq(static_cast<size_t>(recordCount), 0);
    std::vector<uint64_t> parentRef;
    parentRef.reserve(static_cast<size_t>(recordCount));

    uint32_t rootNode = VolumeTree::kInvalid;
    uint32_t recIndex = 0;
    uint64_t recordsProcessed = 0;
    bool cancelled = false;

    ntfs::FileRecord fr;

    auto handleRecord = [&](uint8_t* rec) {
        const uint32_t idx = recIndex++;
        ++recordsProcessed;
        progress.recordsRead.store(recordsProcessed, std::memory_order_relaxed);
        if (progress.cancel.load(std::memory_order_relaxed)) {
            cancelled = true;
            return;
        }
        if (!ntfs::apply_fixups(rec, recSize, bs.bytesPerSector)) return;
        if (!ntfs::parse_file_record(rec, recSize, idx, bs.bytesPerSector, fr)) return;
        if (!fr.inUse) return;
        if (idx < 16 && idx != 5) return;  // NTFS metadata records

        if (idx == 5) {
            rootNode = tree.addNode(volumeRoot.c_str(),
                                    static_cast<uint32_t>(volumeRoot.size()), 0, 0, true, idx);
            parentRef.push_back(0);
            dirNode[5] = rootNode;
            recSeq[5] = fr.sequence;
            return;
        }

        const uint64_t selfSize = fr.isDirectory ? 0 : fr.realSize;
        const uint64_t selfAlloc = fr.isDirectory ? 0 : fr.allocatedSize;
        for (const auto& e : fr.names) {
            if (e.nameSpace != 1 && e.nameSpace != 3) continue;  // Win32 / Win32&DOS only
            const uint32_t node = tree.addNode(e.name, e.nameLength, selfSize, selfAlloc,
                                               fr.isDirectory, idx);
            parentRef.push_back(e.parentRef);
            if (fr.isDirectory && dirNode[idx] == VolumeTree::kInvalid) {
                dirNode[idx] = node;
                recSeq[idx] = fr.sequence;
            }
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
    for (uint32_t i = 0; i < tree.nodeCount(); ++i) {
        if (i == rootNode) continue;
        const uint64_t pref = parentRef[i];
        const uint32_t prec = static_cast<uint32_t>(pref & 0xFFFFFFFFFFFFull);
        const uint16_t pseq = static_cast<uint16_t>(pref >> 48);
        if (prec >= recordCount || prec == i) {
            ++orphanNoParent;
            continue;
        }
        const uint32_t pnode = dirNode[prec];
        if (pnode == VolumeTree::kInvalid) {
            ++orphanNoParent;
            continue;
        }
        if (recSeq[prec] != pseq) {
            ++orphanSeqMismatch;
            continue;
        }
        tree.link(pnode, i);
    }
    progress.orphanNoParent.store(orphanNoParent, std::memory_order_relaxed);
    progress.orphanSeqMismatch.store(orphanSeqMismatch, std::memory_order_relaxed);

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

    tree.setRoot(rootNode);
    tree.aggregate();
    tree.sortChildrenBySizeDesc();
    progress.finished.store(true, std::memory_order_relaxed);
    return true;
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
