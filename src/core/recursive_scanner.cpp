#include "scan.hpp"

#include <windows.h>

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

}  // namespace

bool scan_directory_recursive(const std::wstring& path, VolumeTree& tree,
                              ScanProgress& progress, std::wstring& error) {
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

}  // namespace cs
