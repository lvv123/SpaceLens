#include "scan.hpp"

#include <windows.h>

#include <exception>
#include <new>
#include <string>
#include <vector>

// 本文件是“不需要管理员权限”的备用扫描路径：用 Win32 的 FindFirstFileW /
// FindNextFileW 逐目录枚举，把结果写进同一个 VolumeTree。
// 它不读 $MFT，因此看不到目录索引之外的元数据；作为交换，它在普通用户权限下
// 也能工作，并且可以直接对任意子目录使用（DEV 模式走这条路）。
// 与 MFT 扫描一致，最终必须调用 aggregate() 再排序，UI 才能拿到子树大小。
// 两套扫描器共用 VolumeTree / ScanProgress，UI 不需要区分来源；真正的差异只在
// “能拿到什么信息”：本路径看不到硬链接、没有真实占盘大小、还受权限限制。

namespace cs {
namespace {

// FindFirstFileW 要求通配路径；传入的目录可能带或不带尾部分隔符，
// 这里统一补成 "<dir>\*"（空路径退化为 "*"，即当前目录）。
// 注意没有加 \\?\ 前缀：超过 MAX_PATH 的路径会在这里失败，表现为该目录被
// 静默跳过；这是刻意的取舍，避免为长路径引入额外的 API 分支。
std::wstring join_star(const std::wstring& dir) {
    if (dir.empty()) return L"*";
    const wchar_t last = dir.back();
    if (last == L'\\' || last == L'/') return dir + L"*";
    return dir + L"\\*";
}

// 迭代式（显式栈）深度优先遍历，而不是递归：目录层级可能很深（符号链接环路
// 之外，真实目录也能到上千层），递归会耗尽工作线程的栈空间。
//
// 约定：调用方负责在启动前用 ScanProgress::reset() 清零进度。这里不能再次
// reset，否则会把用户刚设置的 cancel 标志覆盖掉。
bool scan_directory_recursive_impl(const std::wstring& path, VolumeTree& tree,
                                   ScanProgress& progress, std::wstring& error) {
    // 先检查取消：用户可能在上一轮扫描结束后立刻又点了扫描再取消，
    // 此时 cancel 已经置位，必须干净地失败而不是开始遍历。
    if (progress.cancel.load(std::memory_order_relaxed)) {
        tree.clear();
        error = L"扫描已取消";
        progress.finished.store(false, std::memory_order_relaxed);
        progress.failed.store(true, std::memory_order_relaxed);
        return false;
    }

    tree.clear();
    error.clear();

    // 根节点用完整路径作名字（UI 从这一层开始展示）；根节点自身大小记 0，
    // 真实总量由 aggregate() 从子节点汇总上来。
    const uint32_t rootNode =
        tree.addNode(path.c_str(), static_cast<uint32_t>(path.size()), 0, 0, true, 0);
    tree.setRoot(rootNode);

    // 待展开的目录 + 它对应的节点下标。用 vector 当栈，pop_back 顺序即后进先出；
    // 兄弟目录的展开顺序是反的，但这不影响结果（最终会按大小重排）。
    struct Pending {
        std::wstring dir;
        uint32_t node;
    };
    std::vector<Pending> stack;
    stack.push_back({path, rootNode});

    // processed 统计的是“枚举到的目录项数量”，包含目录本身，不区分文件/目录；
    // 它只用于进度显示，因此用 relaxed 原子写即可，不需要与其他字段同步。
    // 另外本路径不填 totalRecords / bytesRead（没有 MFT 总量、也不读原始字节），
    // UI 必须容忍这些计数保持为 0。
    uint64_t processed = 0;
    while (!stack.empty()) {
        if (progress.cancel.load(std::memory_order_relaxed)) break;
        // 取出栈顶目录；move 避免复制长路径字符串。
        Pending cur = std::move(stack.back());
        stack.pop_back();

        const std::wstring pattern = join_star(cur.dir);
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;  // 目录不可读（权限/被独占）：跳过，不中断整次扫描

        do {
            // 这里的 break 只跳出内层 do-while；外层 while 每轮都会重新检查
            // cancel，因此取消后最多再处理完当前这一个目录项就会退出。
            if (progress.cancel.load(std::memory_order_relaxed)) break;
            // "*" 一定会返回 "." 和 ".."，必须显式跳过，否则会自我递归成死循环。
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            // 重解析点（junction / 符号链接 / 挂载点）不展开：跟随它们可能形成环路、
            // 也可能把同一批文件统计两次（甚至跨到别的卷）。
            const bool reparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            // 文件大小是 64 位：Win32 拆成 High/Low 两个 32 位字段，必须拼接而不是
            // 只取低位，否则大于 4 GiB 的文件会被截断。目录不记自身大小。
            uint64_t size = 0;
            if (!isDir) size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;

            // 这里的 alloc 直接传 size：FindFirstFile 不提供“占盘大小”，
            // 所以本路径下 allocated 与 logical 相同（UI 会显示两者一致）。
            // recordIndex 传 0：本路径没有 MFT 记录号可填；该字段只用于诊断与
            // 去重，UI 不应依赖它的具体取值。
            const uint32_t node = tree.addNode(fd.cFileName,
                                               static_cast<uint32_t>(wcslen(fd.cFileName)),
                                               size, size, isDir, 0);
            tree.link(cur.node, node);

            if (isDir && !reparse) {
                // 手工拼子路径而不是用 fd.cFileName 的相对形式：需要绝对路径
                // 才能继续 FindFirstFileW，且要避免出现 "C:\\a\\\\b" 这类双分隔符。
                std::wstring child = cur.dir;
                if (!child.empty() && child.back() != L'\\' && child.back() != L'/')
                    child += L'\\';
                child += fd.cFileName;
                stack.push_back({std::move(child), node});
            }
            ++processed;
            progress.recordsRead.store(processed, std::memory_order_relaxed);
        } while (FindNextFileW(h, &fd));
        // 无论循环因取消还是枚举结束而退出，句柄都必须关闭（提前 break 时也是）。
        FindClose(h);
    }

    // 取消后不发布半成品目录树，也不把本次扫描标记为完成：
    // 否则 UI 会拿到一棵缺少部分分支的树，并把它当成完整结果展示。
    if (progress.cancel.load(std::memory_order_relaxed)) {
        tree.clear();
        error = L"扫描已取消";
        progress.finished.store(false, std::memory_order_relaxed);
        progress.failed.store(true, std::memory_order_relaxed);
        return false;
    }

    // 顺序不能交换：aggregate() 把每个节点的自身大小改写为子树累计大小，
    // sortChildrenBySizeDesc() 依赖这个累计值排序；若先排序，目录会按自身
    // 大小 0 排到末尾。
    tree.aggregate();
    tree.sortChildrenBySizeDesc();
    {
        // 统计文件/目录数量供状态栏展示。这里遍历 arena 全量节点，只读 flags；
        // 未链接到根的节点（本实现不会产生）也会被计入，与 MFT 扫描的统计口径
        // 略有不同，但这些数字仅用于显示。
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

// 对外入口：把异常统一转换成“失败 + 错误文本”。
// 扫描过程中可能因目录规模过大触发内存分配失败，绝不能让 std::bad_alloc 穿透
// 到 GUI 工作线程导致 std::terminate（线程里未捕获异常会直接终止进程）。
// 失败时清空树，保证调用方不会误用半成品。
bool scan_directory_recursive(const std::wstring& path, VolumeTree& tree,
                              ScanProgress& progress, std::wstring& error) {
    try {
        return scan_directory_recursive_impl(path, tree, progress, error);
    } catch (const std::bad_alloc&) {
        tree.clear();
        error = L"内存不足，扫描已中止";
        progress.finished.store(false, std::memory_order_relaxed);
        progress.failed.store(true, std::memory_order_relaxed);
        return false;
    } catch (const std::exception&) {
        // 其他标准异常（如 std::length_error：vector 超过 max_size）同样收敛为
        // 一次失败，而不是把异常抛给 UI 线程。
        tree.clear();
        error = L"扫描时发生内部异常";
        progress.finished.store(false, std::memory_order_relaxed);
        progress.failed.store(true, std::memory_order_relaxed);
        return false;
    }
}

}  // namespace cs
