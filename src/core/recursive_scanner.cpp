#include "scan.hpp"

#include <windows.h>

#include <exception>
#include <new>
#include <string>
#include <vector>

namespace cs {
namespace {

std::wstring join_star(const std::wstring& dir) {
    if (dir.empty()) return L"*";
    const wchar_t last = dir.back();
    if (last == L'\\' || last == L'/') return dir + L"*";
    return dir + L"\\*";
}

bool scan_directory_recursive_impl(const std::wstring& path, VolumeTree& tree,
                                   ScanProgress& progress, std::wstring& error) {
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

    const uint32_t rootNode =
        tree.addNode(path.c_str(), static_cast<uint32_t>(path.size()), 0, 0, true, 0);
    tree.setRoot(rootNode);

    struct Pending {
        std::wstring dir;
        uint32_t node;
    };
    std::vector<Pending> stack;
    stack.push_back({path, rootNode});

    uint64_t processed = 0;
    while (!stack.empty()) {
        if (progress.cancel.load(std::memory_order_relaxed)) break;
        Pending cur = std::move(stack.back());
        stack.pop_back();

        const std::wstring pattern = join_star(cur.dir);
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;  // unreadable directory: skip

        do {
            if (progress.cancel.load(std::memory_order_relaxed)) break;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool reparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            uint64_t size = 0;
            if (!isDir) size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;

            const uint32_t node = tree.addNode(fd.cFileName,
                                               static_cast<uint32_t>(wcslen(fd.cFileName)),
                                               size, size, isDir, 0);
            tree.link(cur.node, node);

            if (isDir && !reparse) {
                std::wstring child = cur.dir;
                if (!child.empty() && child.back() != L'\\' && child.back() != L'/')
                    child += L'\\';
                child += fd.cFileName;
                stack.push_back({std::move(child), node});
            }
            ++processed;
            progress.recordsRead.store(processed, std::memory_order_relaxed);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // 中文注释：取消后不发布半成品目录树，也不把本次扫描标记为完成。
    if (progress.cancel.load(std::memory_order_relaxed)) {
        tree.clear();
        error = L"扫描已取消";
        progress.finished.store(false, std::memory_order_relaxed);
        progress.failed.store(true, std::memory_order_relaxed);
        return false;
    }

    tree.aggregate();
    tree.sortChildrenBySizeDesc();
    {
        uint64_t fileCount = 0, dirCount = 0;
        for (uint32_t i = 0; i < tree.nodeCount(); ++i) {
            if (tree.isDirectory(i)) ++dirCount; else ++fileCount;
        }
        progress.fileCount.store(fileCount, std::memory_order_relaxed);
        progress.dirCount.store(dirCount, std::memory_order_relaxed);
    }
    progress.finished.store(true, std::memory_order_relaxed);
    return true;
}

}  // namespace

bool scan_directory_recursive(const std::wstring& path, VolumeTree& tree,
                              ScanProgress& progress, std::wstring& error) {
    // 中文注释：扫描过程中可能因目录规模过大触发内存分配失败，绝不能让异常
    // 穿透到 GUI 工作线程导致 std::terminate。
    try {
        return scan_directory_recursive_impl(path, tree, progress, error);
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

}  // namespace cs
