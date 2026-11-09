#pragma once

// 扫描入口。
//
// 有两条互不依赖的路径：
// - scan_volume_mft：直接读 $MFT 的快路径，需要管理员权限 + NTFS 卷；
// - scan_directory_recursive：基于 FindFirstFileW 的可移植路径，无需提权，
//   供测试和开发环境（SPACELENS_DEV_DIR）使用。
//
// 线程模型：两条路径都在调用方的后台线程里同步执行；GUI 依靠 ScanProgress
// 里的原子计数器轮询进度，扫描结束后才把 VolumeTree 交给 UI 读取。

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "tree.hpp"

namespace cs {

// 扫描进度与诊断计数。
//
// 全部字段都是 std::atomic：扫描线程写、GUI 线程读，两者之间没有锁。
// 由此带来两条硬性约束：
// 1. 本结构体不可拷贝/赋值（std::atomic 删除了拷贝构造），只能传引用；
// 2. 统计值只是「近似快照」，UI 读到的若干字段可能不是同一时刻的值，
//    不能用它们做需要严格一致性的计算。
struct ScanProgress {
    // 以下为进度类计数，单位是记录/节点条目或字节。
    std::atomic<uint64_t> recordsRead{0};    // MFT：已处理记录数；目录扫描：已处理条目数
    std::atomic<uint64_t> totalRecords{0};   // 仅 MFT 路径能预先得知，目录扫描保持 0
    std::atomic<uint64_t> bytesRead{0};      // 已从卷上实际读取的字节数（不含稀疏补零）
    std::atomic<uint64_t> fileCount{0};      // 扫描结束时统计的文件节点数
    std::atomic<uint64_t> dirCount{0};       // 扫描结束时统计的目录节点数
    // 孤立节点：节点已创建但没能挂到根目录下，通常意味着父记录被回收或损坏。
    // 这些节点的空间仍会被统计，但不会出现在任何目录路径里。
    std::atomic<uint64_t> orphanNodes{0};   // 已创建但未链接到根的节点数
    std::atomic<uint64_t> orphanBytes{0};   // 上述节点的自身大小之和（聚合前的值）
    std::atomic<uint64_t> orphanNoParent{0};   // 未链接：父记录不存在或不是目录
    std::atomic<uint64_t> orphanSeqMismatch{0};  // 未链接：父 FRN 序列号不匹配（记录已回收）
    // orphanNoParent 的细分原因，用于区分数据损坏与正常回收。
    std::atomic<uint64_t> orphanParentOutOfRange{0};   // 父记录号越界或等于自身
    std::atomic<uint64_t> orphanParentNotInUse{0};     // 父记录已标记为删除
    std::atomic<uint64_t> orphanParentNotDir{0};       // 父记录不是目录
    std::atomic<uint64_t> orphanParentNoNode{0};       // 父目录存在但没有对应节点
    // 记录级拒绝原因，合计应当接近 recordsRead，可用于评估卷的健康度。
    std::atomic<uint64_t> recordsNotInUse{0};   // 已删除记录
    std::atomic<uint64_t> recordsMetadata{0};   // recordNumber < 16 且不是根目录(5)的元数据
    std::atomic<uint64_t> recordsNoName{0};     // 没有可用的 $FILE_NAME（无目录项）
    std::atomic<uint64_t> recordsFixupFail{0};  // fixup 校验失败，记录内部结构不可信
    std::atomic<uint64_t> recordsParseFail{0};  // 签名不是 "FILE" 或记录长度不足
    // 大小回填：基础记录的 $DATA 被拆到扩展记录时，去扩展记录取真实大小的次数。
    std::atomic<uint64_t> sizeFollowAttempts{0};
    std::atomic<uint64_t> sizeFollowSuccess{0};
    // 控制标志：cancel 由 UI 线程置位，扫描线程在每条记录前检查一次，
    // 因此取消是「尽力而为」的——正在读的那一块仍会读完。
    std::atomic<bool>     cancel{false};
    std::atomic<bool>     finished{false};   // 仅当整次扫描成功发布结果时才为 true
    std::atomic<bool>     failed{false};     // 失败/取消时为 true，与 finished 互斥

    // 一次扫描开始前把所有计数和状态清零。
    //
    // 逐个字段重置而不是整体赋值，是因为 std::atomic 不可拷贝，无法写
    // `*this = ScanProgress{}`。这里顺带把 cancel 清成 false，所以调用顺序
    // 必须是「先 reset，再由调用方按需设置 cancel」，反过来会被这里抹掉。
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

// 枚举本机可扫描的 NTFS 卷，返回形如 {"C:", "D:"} 的根路径（不含反斜杠）。
// 只保留固定磁盘和可移动磁盘，并且要求文件系统名严格等于 "NTFS"；
// 网络驱动器与 FAT/ReFS 卷不会出现在结果里，因为 $MFT 布局只对 NTFS 成立。
std::vector<std::wstring> list_ntfs_volumes();

// 原始卷字节的来源抽象。真实扫描用卷句柄实现，测试用内存镜像实现，
// 这样 MFT 解析逻辑可以完全脱离管理员权限和真实磁盘来验证。
struct VolumeReader {
    virtual ~VolumeReader() = default;
    // 从卷起始处的字节偏移 offset 读取 size 字节到 buffer。
    // 必须是同步、完整读取：要么读满 size 字节返回 true，要么返回 false，
    // 不支持部分读取——调用方（MFT 解析）依赖缓冲区完整才敢解析。
    virtual bool read(uint64_t offset, void* buffer, uint32_t size) = 0;
};

// 通过 `reader` 扫描一个 NTFS 卷镜像：解析引导扇区、读取 $MFT 自身的 run
// list，再顺序遍历所有 FILE record 并建树。这是 scan_volume_mft 的可测试内核。
//
// 前置条件：progress 已由调用方 reset；reader 的偏移 0 对应卷引导扇区。
// 副作用：会先 clear tree 和 error；失败时 tree 保持为空，error 写入中文原因。
bool scan_mft(VolumeReader& reader, const std::wstring& volumeRoot, VolumeTree& tree,
              ScanProgress& progress, std::wstring& error);

// 直接打开 \\.\X: 扫描整卷的 $MFT。需要提权进程，否则 CreateFileW 会失败，
// 错误信息里会带上 Win32 的原始描述，便于区分「权限不足」和「不是 NTFS」。
// `volumeRoot` 形如 "C:"（不带反斜杠）。
bool scan_volume_mft(const std::wstring& volumeRoot, VolumeTree& tree,
                     ScanProgress& progress, std::wstring& error);

// 用 FindFirstFileW 递归扫描一个目录（无需任何权限）。
// 与 MFT 路径的差异：不跟随 reparse point（避免目录联接成环），无法读取的
// 子目录会被跳过而不是让整次扫描失败；目录节点的自身大小记为 0，其大小由
// aggregate() 从子节点汇总得到。
bool scan_directory_recursive(const std::wstring& path, VolumeTree& tree,
                              ScanProgress& progress, std::wstring& error);

// 把 Win32 错误码格式化成可读消息（FormatMessageW 的系统文案，去掉尾部
// 换行和空格）。系统没有对应文案时回退为「错误码 N」，保证 UI 永远有字可显示。
std::wstring format_win32_error(uint32_t code);

}  // namespace cs
