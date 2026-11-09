// SpaceLens 的 MFT 扫描器：绕过 Win32 目录遍历，直接读 NTFS 卷上的 $MFT，
// 以近乎顺序的 I/O 把整卷的文件/目录结构还原成一棵 VolumeTree。
//
// 扫描严格分成四个阶段，后一阶段依赖前一阶段的产物：
//   1) 读引导扇区，确定每扇区/每簇字节数、FILE record 大小和 $MFT 的起始 LCN；
//      后续所有偏移换算都以这里读出的几何参数为准；
//   2) 用阶段 1 得到的 mftLcn 读出 $MFT 自己的 0 号记录，解码它的 run list，
//      得到「MFT 记录号 -> 卷内物理字节」的映射。没有这张表就无法定位任何
//      一条记录 —— $MFT 自身通常也是碎片化的，不能假设它线性存放；
//   3) 沿这张 run list 分块顺序读取全部记录，逐条做 fixup、解析属性，先把
//      节点以「未链接」的形式追加进 arena；
//   4) 全部记录读完后统一建树：校验父引用、把节点挂到父目录，再聚合子树
//      大小并按大小降序排序。
//
// 阶段 3 只建节点、阶段 4 才链接，是因为一条记录的父目录完全可能排在该
// 记录之后；边读边挂会引入二次遍历，分两遍反而更简单、缓存也更友好。
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

// 卷句柄的 RAII 包装：scan_volume_mft 里任何一条提前 return 都必须关句柄，
// 交给析构函数处理比在每处手写 CloseHandle 更难漏。
struct HandleGuard {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~HandleGuard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    HandleGuard() = default;
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
};

// 通过真实卷句柄（\\.\X:）读取原始字节的 VolumeReader 实现。
// 卷设备句柄不能像普通文件那样假设当前文件指针可用，每次读取前都显式
// SetFilePointerEx 定位是最可靠的做法。
struct HandleReader : VolumeReader {
    HANDLE h = INVALID_HANDLE_VALUE;

    // offset 是卷内绝对字节偏移（不是 cluster 号），换算由调用方负责。
    bool read(uint64_t offset, void* buf, uint32_t size) override {
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
        DWORD total = 0;
        auto* p = static_cast<uint8_t*>(buf);
        // ReadFile 允许短读（跨 cluster 边界、被分页打断、设备只返回一部分），
        // 必须循环读满；否则调用方会拿到半条 FILE record，后续 fixup 校验
        // 会莫名其妙地失败。
        while (total < size) {
            DWORD got = 0;
            if (!ReadFile(h, p + total, size - total, &got, nullptr)) return false;
            // got == 0 却没有报错，通常意味着介质被移除；继续循环会死等。
            if (got == 0) return false;
            total += got;
        }
        return true;
    }
};

}  // namespace

// 把 Win32 错误码翻译成系统语言的可读消息，用于拼装面向用户的错误文案。
// FORMAT_MESSAGE_ALLOCATE_BUFFER 让系统自行分配缓冲区，因此用完必须
// LocalFree，不能交给 delete/delete[]。
std::wstring format_win32_error(uint32_t code) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring s;
    if (n != 0 && buf != nullptr) {
        s.assign(buf, n);
        // 系统消息末尾通常带 \r\n，直接贴到 UI 上会多出空行，这里统一裁掉。
        while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' '))
            s.pop_back();
    } else {
        // 错误码没有对应消息（或缺少语言包）时退化为纯数字，至少不会把空串
        // 当成错误原因展示。
        s = L"错误码 " + std::to_wstring(code);
    }
    if (buf != nullptr) LocalFree(buf);
    return s;
}

// 枚举本机所有「固定盘 / 可移动盘」且文件系统恰好是 NTFS 的盘符。
// 网络映射盘（DRIVE_REMOTE）被排除：它们不暴露 $MFT，拼成 \\.\X: 也没有意义。
// 返回值形如 {"C:", "D:"}，不带反斜杠，调用方自行拼设备名或根路径。
std::vector<std::wstring> list_ntfs_volumes() {
    std::vector<std::wstring> out;
    // GetLogicalDrives 返回 26 位位图，第 i 位对应盘符 'A' + i。
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        const wchar_t letter = static_cast<wchar_t>(L'A' + i);
        wchar_t root[4] = {letter, L':', L'\\', 0};
        // 只考虑能本地寻址的卷类型；光驱、网络盘、未挂载设备一律跳过。
        const UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        wchar_t fsName[MAX_PATH + 1] = {};
        // 取文件系统名可能因权限不足或介质不在而失败，失败即跳过该盘。
        // 缓冲区比 MAX_PATH 多 1，保证结果一定以 NUL 结尾。
        if (GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fsName,
                                  MAX_PATH + 1) == FALSE)
            continue;
        // 只认 NTFS：FAT/exFAT/ReFS 没有这里要解析的 $MFT 结构。
        if (wcscmp(fsName, L"NTFS") == 0)
            out.push_back(std::wstring(1, letter) + L":");
    }
    return out;
}

// 扫描的实现主体。四个阶段顺序执行，任一阶段失败都通过 error + progress.failed
// 反馈给调用方。开始前先确认 reader 能读到偏移 0（引导扇区），读不到就说明
// 这个 VolumeReader 根本不对应一个可读的 NTFS 卷。
static bool scan_mft_impl(VolumeReader& reader, const std::wstring& volumeRoot,
                          VolumeTree& tree, ScanProgress& progress, std::wstring& error) {
    // 调用方负责在启动前用 ScanProgress::reset() 清零进度。这里不能再次
    // reset，否则会把用户刚按下的 cancel 标志覆盖掉。
    // 启动前就已取消时也不要假装成功：写入 failed，让 UI 走明确的错误分支。
    if (progress.cancel.load(std::memory_order_relaxed)) {
        tree.clear();
        error = L"扫描已取消";
        progress.finished.store(false, std::memory_order_relaxed);
        progress.failed.store(true, std::memory_order_relaxed);
        return false;
    }

    // 复用的 VolumeTree 可能还留着上一次扫描的 arena，先清空，保证本次结果
    // 从零开始，也避免旧节点被本轮的 link 误引用。
    tree.clear();
    error.clear();

    // ---- 阶段 1：读引导扇区，确定这个卷的几何参数 ----
    // 引导扇区在卷偏移 0 处，实际只有 512 字节；这里多读一些凑满一页，
    // 解析器只关心最前面的 512 字节。读失败通常是句柄没有读权限（未提权）
    // 或介质已被移除。
    uint8_t boot[4096];
    if (!reader.read(0, boot, sizeof(boot))) {
        error = L"读取引导扇区失败";
        progress.failed = true;
        return false;
    }
    ntfs::BootSector bs;
    // 校验 OEM 标识、每扇区/每簇字节数、FILE record 大小等字段是否合法且为
    // 2 的幂；任一项异常都说明这不是 NTFS，或者数据已损坏，继续解析只会
    // 把垃圾当成记录。
    if (!ntfs::parse_boot_sector(boot, sizeof(boot), bs)) {
        error = L"不是有效的 NTFS 卷（引导扇区校验失败）";
        progress.failed = true;
        return false;
    }

    // FILE record 的常见大小是 1KB/4KB，簇极大的卷上也可能是 2KB。
    // 超出 [64, 4096] 说明引导扇区里的编码位被改坏，按它切片会越界。
    const uint32_t recSize = bs.bytesPerFileRecord;
    if (recSize < 64 || recSize > 4096) {
        error = L"异常的 MFT 记录大小";
        progress.failed = true;
        return false;
    }

    // ---- 阶段 2：读 $MFT 自己的 FILE record，解码它的 run list ----
    // $MFT 是卷上的 0 号记录，而这条记录本身又描述了「MFT 存放在哪些簇」：
    // 它的未命名 $DATA 就是整张 MFT 表。必须先拿到这张表，才能按记录号
    // 定位其他任何记录，所以这一步是后面所有随机读的前提。
    //
    // 记录大小不一定是扇区大小的整数倍，向上取整到扇区边界再读：卷设备对
    // 非对齐长度读取的行为依赖底层存储栈，凑齐整扇区更稳妥。
    const uint32_t align = std::max<uint32_t>(bs.bytesPerSector, 512);
    const uint32_t readLen = ((recSize + align - 1) / align) * align;
    std::vector<uint8_t> mftRec(readLen, 0);
    // mftLcn 是 $MFT 第 0 个字节所在簇；乘以每簇字节数才是卷内绝对偏移。
    const uint64_t mftOffset = static_cast<uint64_t>(bs.mftLcn) * bs.bytesPerCluster;
    if (!reader.read(mftOffset, mftRec.data(), readLen)) {
        error = L"读取 $MFT 记录失败";
        progress.failed = true;
        return false;
    }
    // fixup（update sequence array）是 NTFS 的撕裂写检测：每条记录每个扇区
    // 末尾的两个字节在落盘时被换成 USN，读回后必须还原。不先做 fixup，
    // 后面解出的属性头几乎必然是错的。顺序必须是「先 fixup，再解 run list」。
    if (!ntfs::apply_fixups(mftRec.data(), recSize, bs.bytesPerSector)) {
        error = L"$MFT 记录校验失败";
        progress.failed = true;
        return false;
    }
    std::vector<ntfs::Run> runs;
    uint64_t mftReal = 0;
    uint64_t mftAlloc = 0;
    // 取出未命名 $DATA（必然非常驻）的 run list，同时得到 MFT 的逻辑长度和
    // 磁盘占用长度。mftReal 才是有效记录的边界。
    if (!ntfs::decode_data_attribute(mftRec.data(), recSize, runs, mftReal, mftAlloc)) {
        error = L"无法解析 $MFT 数据分布表";
        progress.failed = true;
        return false;
    }

    // 记录数用「逻辑大小 / 记录大小」而不是分配大小：MFT 尾部常有一段已分配
    // 但从未使用过的预留空间，按逻辑大小可以跳过它们。空 MFT 说明卷尚未
    // 完成初始化，没有继续扫描的意义。
    const uint64_t recordCount = mftReal / recSize;
    progress.totalRecords.store(recordCount, std::memory_order_relaxed);
    if (recordCount == 0) {
        error = L"MFT 为空";
        progress.failed = true;
        return false;
    }
    // 目录索引、记录状态等旁路数组按 uint32 记录号索引；超过 2^32-1 条记录
    // 既无法表示，也会让后面的 size_t 乘法溢出，必须提前拒绝。
    if (recordCount > 0xFFFFFFFFull) {
        error = L"MFT 记录数量超出支持范围";
        progress.failed = true;
        return false;
    }

    // 一次性预留节点与名称池：节点数通常接近记录数，硬链接只会让它更多，
    // 所以这里给的是下界而非上限。名称池 16 字符/节点只是经验估值。
    // 预留的意义是避免在百万级循环里反复扩容、搬移整个 arena。
    tree.reserve(static_cast<size_t>(recordCount), static_cast<size_t>(recordCount) * 16u);

    // 三张按「MFT 记录号」直接索引的旁路表，供阶段 4 挂接节点：
    //   dirNode[idx]   —— 该记录作为目录时，其代表节点的下标（目录只保留一个）；
    //   recSeq[idx]    —— 该记录的序列号，用于校验父引用指向的是同一条记录；
    //   recStatus[idx] —— 位标记：1=inUse，2=目录，4=已建出节点。
    // 一个目录可能因硬链接有多个节点，但作父节点只能选一个，这里取第一个
    // 成功建立的目录节点。
    std::vector<uint32_t> dirNode(static_cast<size_t>(recordCount), VolumeTree::kInvalid);
    std::vector<uint16_t> recSeq(static_cast<size_t>(recordCount), 0);
    // 与树节点下标一一对应的父引用（完整 64 位 FRN）。节点建立时父目录可能
    // 还没被读到，所以先暂存，等全部记录读完再统一解析。
    std::vector<uint64_t> parentRef;
    parentRef.reserve(static_cast<size_t>(recordCount));

    uint64_t rootNode = VolumeTree::kInvalid;
    uint32_t recIndex = 0;
    uint64_t recordsProcessed = 0;
    // 跟随 $ATTRIBUTE_LIST 找 $DATA 的次数与成功次数，用来衡量卷上有多少
    // 大文件/碎片文件走了这条慢路径。
    uint64_t sizeFollowAttempts = 0;
    uint64_t sizeFollowSuccess = 0;
    bool cancelled = false;

    ntfs::FileRecord fr;
    ntfs::FileRecord frExt;
    std::vector<uint8_t> attrListBuf;

    // 把「MFT 记录号」映射为卷内物理偏移并读出一条 FILE record，用于跟随
    // $ATTRIBUTE_LIST 找到持有 $DATA 的扩展记录。
    // 映射依赖阶段 2 解出的 runs：MFT 自身往往也是碎片化的，绝不能假设
    // 「记录号 * recSize」就是线性物理地址。
    // 这类随机读只占极少数（仅大文件/碎片文件），单独定位比打断顺序读更划算。
    auto read_record = [&](uint64_t recordNumber, uint8_t* buf) -> bool {
        // recordNumber * recSize 是 MFT 这条逻辑流内的字节偏移，需要在 run list
        // 中找到覆盖它的那个 extent。
        const uint64_t streamOffset = recordNumber * recSize;
        for (const auto& run : runs) {
            // run.vcn 是逻辑簇号，乘簇大小才是该 extent 在流内的起始字节。
            const uint64_t runStart = run.vcn * bs.bytesPerCluster;
            const uint64_t runBytes = run.lengthClusters * bs.bytesPerCluster;
            // 不在本 extent 范围内就找下一个；extent 之间不保证物理连续。
            if (streamOffset < runStart || streamOffset >= runStart + runBytes) continue;
            // 稀疏 extent 表示这部分逻辑流没有落盘数据，读出来只会是 0；
            // 直接失败比返回一条假记录安全。
            if (run.sparse) return false;
            // extent 内偏移 + 该 extent 的物理起点 = 卷内绝对偏移。
            const uint64_t phys = run.lcn * bs.bytesPerCluster + (streamOffset - runStart);
            return reader.read(phys, buf, recSize);
        }
        return false;
    };

    // 把一个由 `streamRuns` 描述的流读进 `out`，最多读 `maxBytes` 字节。
    // 这里用于 $ATTRIBUTE_LIST：它可能非常驻，也可能被拆成多个 extent。
    // 上限由调用方给，避免损坏数据里夸大的长度把内存吃光。
    auto read_stream = [&](const std::vector<ntfs::Run>& streamRuns, uint64_t maxBytes,
                           std::vector<uint8_t>& out) -> bool {
        out.clear();
        for (const auto& run : streamRuns) {
            const uint64_t runBytes = run.lengthClusters * bs.bytesPerCluster;
            // 稀疏 extent 的 lcn 没有意义，这个偏移只在非稀疏分支被使用。
            uint64_t off = run.lcn * bs.bytesPerCluster;
            uint64_t left = runBytes;
            while (left > 0 && out.size() < maxBytes) {
                // 每次最多只取到 maxBytes 上限，不整条流读完。
                const size_t n =
                    static_cast<size_t>(std::min<uint64_t>(left, maxBytes - out.size()));
                const size_t old = out.size();
                out.resize(old + n);
                if (run.sparse) {
                    // 稀疏区域补 0，让后续解析看到的是「空属性列表项」。
                    std::memset(out.data() + old, 0, n);
                } else if (!reader.read(off, out.data() + old, static_cast<uint32_t>(n))) {
                    // 读失败向上冒泡；调用方会退回到基础记录自带的大小。
                    return false;
                }
                off += n;
                left -= n;
            }
            if (out.size() >= maxBytes) break;
        }
        return true;
    };

    // 每条记录的位标记，按记录号索引：1=inUse，2=目录，4=已成功建出节点。
    // 阶段 4 校验父引用时只看这张表，不必回头重新解析记录。
    std::vector<uint8_t> recStatus(static_cast<size_t>(recordCount), 0);
    // 分类计数：fixup 失败 / 解析失败 / 已删除 / 元数据 / 无可用名称。
    // 这些值原样写进 ScanProgress，用来解释「记录总数和树里的节点数为何对不上」。
    uint64_t cNotInUse = 0, cMetadata = 0, cNoName = 0, cFixupFail = 0, cParseFail = 0;

    // 处理一条已拼接完整的 FILE record：fixup -> 解析 -> 建出（尚未链接的）
    // 节点。节点的粒度是「一个目录项」，因此硬链接会得到多个节点，但它们
    // 共享同一个 recordIndex。
    //
    // 这里刻意不调用 link()：父目录可能出现在后面的记录里，统一留到阶段 4。
    auto handleRecord = [&](uint8_t* rec) {
        // idx 是本条记录在 MFT 中的槽位号（也是 parse_file_record 需要的
        // recordNumber）。recIndex++ 同时充当上面读循环的「下一条待处理」游标。
        const uint32_t idx = recIndex++;
        ++recordsProcessed;
        // 进度按「已从磁盘读出的记录数」推进：即使这条随后被过滤掉
        // （已删除 / 元数据），它也确实被读出来了，所以计数在过滤之前 +1。
        progress.recordsRead.store(recordsProcessed, std::memory_order_relaxed);
        // 取消只在记录边界检查：单条记录的处理是微秒级的，粒度足够，
        // 比在每个属性上访问一次原子变量便宜得多。
        if (progress.cancel.load(std::memory_order_relaxed)) {
            cancelled = true;
            return;
        }
        // fixup 失败 = 记录被撕裂写或磁盘坏块；继续解析只会得到垃圾数据。
        if (!ntfs::apply_fixups(rec, recSize, bs.bytesPerSector)) { ++cFixupFail; return; }
        // 签名/边界校验失败。注意「解析失败」与「已删除」是两回事：已删除
        // 记录的签名仍然有效，能正常解析。
        if (!ntfs::parse_file_record(rec, recSize, idx, bs.bytesPerSector, fr)) { ++cParseFail; return; }
        // 已删除记录仍占着 MFT 槽位，但它引用的数据可能已被别的文件复用。
        // 直接丢弃，绝不能进树，否则会出现幽灵文件和错误的大小。
        if (!fr.inUse) { ++cNotInUse; return; }
        // NTFS 的 0..15 号记录是 $MFT/$MFTMirr/$LogFile/$Volume/$AttrDef/
        // 根目录/$Bitmap/$Boot/$BadClus/$Secure/$UpCase/$Extend 等元数据。
        // 记录 5（根目录）是唯一例外：它是整棵树的挂载点，必须按真实目录处理。
        if (idx < 16 && idx != 5) { ++cMetadata; return; }

        // 位标记留给阶段 4：父引用指向的必须是 inUse 的目录，且该目录已经
        // 有代表节点，否则子节点会被归入孤儿统计。
        recStatus[idx] |= 0x01;
        if (fr.isDirectory) recStatus[idx] |= 0x02;

        if (idx == 5) {
            // 根目录：节点名直接用卷根（如 "C:"），大小记 0，由 aggregate()
            // 汇总出子树总量。parentRef 塞 0 占位，因为根节点在阶段 4 被跳过。
            // 记录 5 也可能晚于其他记录被读到，下面的按索引写入保证两种顺序
            // 都能正确处理。
            rootNode = tree.addNode(volumeRoot.c_str(),
                                    static_cast<uint32_t>(volumeRoot.size()), 0, 0, true, idx);
            parentRef.push_back(0);
            dirNode[5] = static_cast<uint32_t>(rootNode);
            recSeq[5] = fr.sequence;
            recStatus[5] |= 0x04;
            return;
        }

        // 目录的「自身大小」记 0：目录不占 $DATA，其子树大小完全由 aggregate()
        // 累加得出；提前填一个数会导致重复计算。
        uint64_t selfSize = fr.isDirectory ? 0 : fr.realSize;
        uint64_t selfAlloc = fr.isDirectory ? 0 : fr.allocatedSize;

        // 大文件/碎片文件会把未命名 $DATA 放进由 $ATTRIBUTE_LIST 列出的扩展
        // 记录里。此时基础记录没有 $DATA，realSize 为 0，而且基础记录的
        // $FILE_NAME 大小同样是 0 —— 不能拿它当回退值。
        //
        // 处理顺序：优先用解析时已经看出的 extent 引用；否则读取（可能非常驻
        // 的）$ATTRIBUTE_LIST，按「未命名 $DATA 中 VCN 最低」的规则找出真正
        // 持有数据的记录号。
        if (!fr.isDirectory && selfSize == 0) {
            uint64_t dataRef = 0;
            bool haveRef = fr.dataExtentRefValid;
            if (haveRef) {
                // parse 阶段直接从基础记录里看到「$DATA 在别处」，省掉一次
                // $ATTRIBUTE_LIST 的读取。
                dataRef = fr.dataExtentRef;
            } else if (fr.hasAttrListRuns) {
                // 属性列表项通常十几到几十字节，前 256KB 足以覆盖绝大多数
                // 文件；同时也是对损坏数据的硬上限，防止内存被撑爆。
                const uint64_t want = std::min<uint64_t>(fr.attrListRealSize, 256u * 1024u);
                if (read_stream(fr.attrListRuns, want, attrListBuf) && !attrListBuf.empty()) {
                    uint64_t ref = 0;
                    // 只取 VCN 最低的那个未命名 $DATA：它才是文件起始 extent，
                    // 高 VCN 的只是后续片段，单独读会得到错误的大小和 run list。
                    if (ntfs::find_data_extent_in_attribute_list(
                            attrListBuf.data(), static_cast<uint32_t>(attrListBuf.size()), ref)) {
                        dataRef = ref;
                        haveRef = true;
                    }
                }
            }
            if (haveRef) {
                // 引用低 48 位是记录号，高 16 位是序列号，这里只需要记录号。
                // refRec == idx 表示列表指向自己，属于损坏数据，必须跳过。
                const uint64_t refRec = dataRef & 0xFFFFFFFFFFFFull;
                if (refRec != idx && refRec < recordCount) {
                    ++sizeFollowAttempts;
                    uint8_t extBuf[4096];
                    // 扩展记录同样要「先 fixup、再解析」。这里用独立的 frExt，
                    // 因为下面还要继续使用基础记录的 names/sequence。
                    if (read_record(refRec, extBuf) &&
                        ntfs::apply_fixups(extBuf, recSize, bs.bytesPerSector) &&
                        ntfs::parse_file_record(extBuf, recSize, static_cast<uint32_t>(refRec),
                                                bs.bytesPerSector, frExt) &&
                        // 只接受非 0 大小：0 通常说明这条只是持有 $ATTRIBUTE_LIST
                        // 的中间记录，并不是真正存放 $DATA 的那条。
                        frExt.realSize != 0) {
                        selfSize = frExt.realSize;
                        selfAlloc = frExt.allocatedSize;
                        ++sizeFollowSuccess;
                    }
                }
            }
        }

        // 为每个父目录挑一个名称。一条记录可能有多个 $FILE_NAME（硬链接），
        // 每个父目录项都会成为一个独立节点，所以这里按「父记录号」去重，
        // 而不是按完整 64 位 FRN —— 同一父目录下的多个名称只差序列号位。
        //
        // 命名空间取舍：1(Win32) 与 3(Win32&DOS) 是资源管理器展示的名字，
        // 排位最高；2(DOS) 是 8.3 短名；0(POSIX) 是区分大小写的长名。
        // 有高优先级名称时用高优先级；只有低优先级时也必须保留，否则用
        // POSIX 语义工具创建的文件会整棵子树变成孤儿。
        uint64_t chosenKey[32];
        size_t chosenIdx[32];
        uint8_t chosenRank[32];
        size_t chosenCount = 0;
        for (size_t a = 0; a < fr.names.size(); ++a) {
            const auto& e = fr.names[a];
            // 长度为 0 的名称没有意义（损坏记录或占位项）。
            if (e.nameLength == 0) continue;
            // 只用低 48 位做键：序列号不同但记录号相同的两个名称，仍属于
            // 同一个父目录，应当合并到同一个节点槽位。
            const uint64_t key = e.parentRef & 0xFFFFFFFFFFFFull;
            // 排位 0 优于排位 1，只有 Win32 命名空间能拿到 0。
            const uint8_t rank = (e.nameSpace == 1 || e.nameSpace == 3) ? 0 : 1;
            size_t slot = chosenCount;
            // 线性扫描已选表（最多 32 项）；一条记录的 $FILE_NAME 数量很少，
            // 用哈希表反而更慢。
            for (size_t k = 0; k < chosenCount; ++k) {
                if (chosenKey[k] == key) { slot = k; break; }
            }
            if (slot == chosenCount) {
                // 32 是保护性上限：损坏记录可能声称有极多名称，超出部分丢弃。
                if (chosenCount >= 32) continue;
                chosenKey[chosenCount] = key;
                chosenIdx[chosenCount] = a;
                chosenRank[chosenCount] = rank;
                ++chosenCount;
            } else if (rank < chosenRank[slot]) {
                // 同一父目录已有候选时，用排位更小（更友好）的名字替换。
                chosenIdx[slot] = a;
                chosenRank[slot] = rank;
            }
        }

        // 每个父目录项都建一个节点：这就是硬链接的语义 —— 同一个 MFT 记录在
        // 多个目录下各出现一次，各自计入一次大小，与资源管理器一致；它们
        // 共享 recordIndex，UI 可以据此识别出是同一份数据。
        bool created = false;
        for (size_t k = 0; k < chosenCount; ++k) {
            const auto& e = fr.names[chosenIdx[k]];
            const uint32_t node = tree.addNode(e.name, e.nameLength, selfSize, selfAlloc,
                                               fr.isDirectory, idx);
            // parentRef 必须与节点下标严格同步追加；错一格就会把整棵树挂错。
            parentRef.push_back(e.parentRef);
            created = true;
            // 目录只登记第一个节点作为代表节点：父引用需要一个能挂孩子的
            // 节点，多出来的硬链接节点不适合充当代表。
            if (fr.isDirectory && dirNode[idx] == VolumeTree::kInvalid) {
                dirNode[idx] = node;
                recSeq[idx] = fr.sequence;
            }
        }
        if (created) {
            recStatus[idx] |= 0x04;
        } else {
            // 一条记录一个节点都建不出来（名称全为空）：它在树里不存在，
            // 也就不会参与孤儿统计。
            ++cNoName;
        }
    };

    // ---- 阶段 3：沿 $MFT 的 run list 分块顺序读取全部记录 ----
    // 分块大小取 8MB：太小会让每次 read() 的系统调用与设备寻道开销占比过高；
    // 太大则延长取消的响应时间，并让内存里多压一份数据。8MB 在 SSD 和机械盘
    // 上都是比较均衡的取值。
    //
    // 块大小必须是 cluster 的整数倍，否则块边界会切断 cluster，与 run list
    // 的簇对齐假设冲突；当簇本身比 8MB 还大时退化为一个簇。
    uint64_t chunkBytes = 8ull << 20;
    if (chunkBytes < bs.bytesPerCluster) {
        chunkBytes = bs.bytesPerCluster;
    } else {
        chunkBytes -= chunkBytes % bs.bytesPerCluster;
    }
    std::vector<uint8_t> chunk(static_cast<size_t>(chunkBytes));
    // carry 用于跨块拼接「被块边界切断的记录」：记录大小不一定整除块大小，
    // 一条记录可能前半段在上一块末尾、后半段在下一块开头。
    // 预留 4 条记录的空间，正常情况下里面最多残留 1 条不完整记录。
    std::vector<uint8_t> carry;
    carry.reserve(recSize * 4);
    size_t carryPos = 0;

    // 把一块原始字节喂给解析循环：先接上上一块的残留，再按记录大小逐条切分。
    // carryPos 记录「已经处理到 carry 的哪个字节」，而不是每次 erase 掉已消费
    // 部分 —— 避免每块都搬移一次剩余数据。
    auto feed = [&](const uint8_t* data, size_t n) {
        // 上一轮结束后丢掉已消费的前缀，只保留不足一条记录的尾巴。
        if (carryPos > 0) {
            carry.erase(carry.begin(), carry.begin() + static_cast<std::ptrdiff_t>(carryPos));
            carryPos = 0;
        }
        carry.insert(carry.end(), data, data + n);
        size_t i = 0;
        // 只有「剩余字节 >= 一条记录」时才处理；recIndex 同时是记录游标，
        // 读到 recordCount 就停（尾部预留的槽位不读）。
        while (recIndex < recordCount && carry.size() - i >= recSize && !cancelled) {
            handleRecord(carry.data() + i);
            i += recSize;
        }
        // 不完整的那截留到下一块，carryPos 指向这里，等后续字节补齐。
        // 取消后也走这里，但外层读循环会立刻退出，不再调用 feed。
        carryPos = i;
    };

    bool readFailed = false;
    // 外层遍历 run、内层把每个 extent 切成 chunkBytes 的块。这样既能顺序
    // 预读（FILE_FLAG_SEQUENTIAL_SCAN 才会生效），又不必一次性分配等于整张
    // MFT 大小的缓冲区 —— 大卷上那可能是几百 MB 甚至 GB。
    for (const auto& run : runs) {
        // 记录已读完 / 已取消 / 已读失败时都没必要再碰磁盘。
        if (cancelled || readFailed || recIndex >= recordCount) break;
        const uint64_t runBytes = run.lengthClusters * bs.bytesPerCluster;
        // extent 之间物理上不连续，每个 extent 的起点与剩余量都要重新计算，
        // 不能把上一个 extent 的偏移顺延下来。
        uint64_t off = run.lcn * bs.bytesPerCluster;
        uint64_t left = runBytes;
        while (left > 0 && !cancelled && recIndex < recordCount) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(left, chunkBytes));
            if (run.sparse) {
                // 稀疏 extent（$MFT 里理论上不该出现）按 0 填充，这样记录
                // 切片仍然对齐，parse 时会被签名校验挡掉。
                std::memset(chunk.data(), 0, n);
            } else if (!reader.read(off, chunk.data(), static_cast<uint32_t>(n))) {
                readFailed = true;
                break;
            } else {
                // 只有真正从设备读到字节才累加；稀疏填充不算 I/O。
                progress.bytesRead.fetch_add(n, std::memory_order_relaxed);
            }
            // 传 n 而不是整块 chunk：最后一个块通常不满。
            feed(chunk.data(), n);
            off += n;
            left -= n;
        }
    }

    // 取消不等于成功：已经建出的半成品树必须丢弃，并通过 failed 明确告知 UI，
    // 否则界面会显示一份看似完整、实际缺了后半段的扫描结果。
    // 这里再查一次 cancel，是因为读循环可能在上一次 feed 返回后才观察到取消。
    if (cancelled || progress.cancel.load(std::memory_order_relaxed)) {
        tree.clear();
        error = L"扫描已取消";
        progress.finished.store(false, std::memory_order_relaxed);
        progress.failed.store(true, std::memory_order_relaxed);
        return false;
    }
    // 读到一半失败（介质错误、设备被拔出）时保留已有进度没有意义：大小聚合
    // 会基于残缺数据得出错误结论，直接报错更诚实。
    if (readFailed) {
        error = L"读取 MFT 数据时失败";
        progress.failed = true;
        return false;
    }
    // 记录 5 从未出现（或它自身 fixup/parse 失败）时整卷没有挂载点，树无法
    // 成立；必须报错，而不是返回一棵全是孤儿的树。
    if (rootNode == VolumeTree::kInvalid) {
        error = L"未能定位卷根目录记录";
        progress.failed = true;
        return false;
    }

    // ---- 阶段 4：建树 —— 把节点挂到父目录，并校验父引用的有效性 ----
    // 父引用是完整的 64 位 FRN：低 48 位记录号 + 高 16 位序列号。
    // 序列号必须校验：MFT 槽位会被回收复用，若只比对记录号，一个已删除文件
    // 残留的引用可能恰好指向「现在占用该槽位的另一个目录」，把节点挂到完全
    // 无关的目录下，甚至形成环。
    //
    // 孤儿分两类统计：
    //   orphanNoParent    —— 父引用本身不可用（越界 / 已删除 / 不是目录 / 没建节点）；
    //   orphanSeqMismatch —— 父槽位已属于另一条记录（序列号不符）。
    // 下面的 op* 只是细分诊断，用来判断哪种损坏更常见，UI 上仍展示汇总值。
    uint64_t orphanNoParent = 0;
    uint64_t orphanSeqMismatch = 0;
    uint64_t opOutOfRange = 0, opNotInUse = 0, opNotDir = 0, opNoNode = 0;
    for (uint32_t i = 0; i < tree.nodeCount(); ++i) {
        // 节点下标 0 不一定是根，必须用 rootNode 显式跳过；根节点不参与链接。
        if (i == rootNode) continue;
        const uint64_t pref = parentRef[i];
        // 低 48 位是父记录号，高 16 位是父记录创建时的序列号。
        const uint32_t prec = static_cast<uint32_t>(pref & 0xFFFFFFFFFFFFull);
        const uint16_t pseq = static_cast<uint16_t>(pref >> 48);
        // 越界 = 引用指向不存在的槽位，访问 dirNode/recStatus 会越界；
        // prec == i 是自引用（损坏），会让 link() 成环。必须在挂接前挡掉。
        if (prec >= recordCount || prec == i) {
            ++orphanNoParent;
            ++opOutOfRange;
            continue;
        }
        // 以下三种情况父记录都不可用：已删除（未使用）、不是目录、或者当初
        // 因为它自身解析失败而没有建出代表节点。
        const uint8_t st = recStatus[prec];
        if ((st & 0x01) == 0) { ++orphanNoParent; ++opNotInUse; continue; }
        if ((st & 0x02) == 0) { ++orphanNoParent; ++opNotDir; continue; }
        const uint32_t pnode = dirNode[prec];
        if (pnode == VolumeTree::kInvalid) { ++orphanNoParent; ++opNoNode; continue; }
        // 序列号不符：槽位已被回收给别的记录，绝不能把本节点挂过去。
        if (recSeq[prec] != pseq) {
            ++orphanSeqMismatch;
            continue;
        }
        // 通过全部检查，可以安全链接；link 只改指针，排序留到最后统一做。
        tree.link(pnode, i);
    }
    // 把本地计数器一次性发布到 ScanProgress。全部使用 relaxed 序：这些值只
    // 用于展示进度与诊断，不需要与树结构建立 happens-before 关系；表示扫描
    // 结束的信号是最后那个 finished 标志。
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

    // 诊断汇总：节点总数、文件/目录数，以及「一个父节点都没挂上」的孤儿节点
    // 及其自身大小之和。
    // 注意此时 aggregate() 还没跑，totalSize 仍然只是每个节点自己的大小，
    // 所以 orphanBytes 统计的是孤儿节点自身字节数，不含它们的子树。
    {
        uint64_t fileCount = 0, dirCount = 0, orphans = 0, orphanBytes = 0;
        for (uint32_t i = 0; i < tree.nodeCount(); ++i) {
            const auto& n = tree.node(i);
            if (tree.isDirectory(i)) ++dirCount; else ++fileCount;
            // 根节点本来就没有父节点，跳过它是为了不虚报孤儿。
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

    // 收尾顺序不能变：先确定根，再自底向上聚合子树大小，最后按大小降序排列
    // 兄弟链（UI 只做逐层下钻，依赖这个顺序直接从头遍历）。
    // finished 必须是最后一步：它一旦置位，UI 就可能开始读取这棵树。
    tree.setRoot(static_cast<uint32_t>(rootNode));
    tree.aggregate();
    tree.sortChildrenBySizeDesc();
    progress.finished.store(true, std::memory_order_relaxed);
    return true;
}

// 对外入口：包一层异常边界。磁盘数据损坏或卷容量异常可能让 vector 扩容抛出
// bad_alloc/length_error；这里统一转换成错误结果，避免异常逃出 GUI 工作线程
// 触发 std::terminate。
bool scan_mft(VolumeReader& reader, const std::wstring& volumeRoot, VolumeTree& tree,
              ScanProgress& progress, std::wstring& error) {
    try {
        return scan_mft_impl(reader, volumeRoot, tree, progress, error);
    } catch (const std::bad_alloc&) {
        // 内存不足时 tree 可能只建了一半，先清掉再报告，保证失败不留垃圾。
        tree.clear();
        error = L"内存不足，扫描已中止";
        progress.finished.store(false, std::memory_order_relaxed);
        progress.failed.store(true, std::memory_order_relaxed);
        return false;
    } catch (const std::exception&) {
        // 其余标准异常（越界、长度错误等）同样不能穿过线程边界；统一成一句
        // 用户能理解的错误，避免把内部异常类型泄漏到界面上。
        tree.clear();
        error = L"扫描时发生内部异常";
        progress.finished.store(false, std::memory_order_relaxed);
        progress.failed.store(true, std::memory_order_relaxed);
        return false;
    }
}

// 打开卷设备后走 MFT 快路径。设备名形如 \\.\C:，与普通文件路径不同：
// 它要求进程具备管理员权限，且 FILE_SHARE_READ|WRITE 必须给全，否则卷上有
// 其他进程正在写时就会打不开。
bool scan_volume_mft(const std::wstring& volumeRoot, VolumeTree& tree,
                     ScanProgress& progress, std::wstring& error) {
    const std::wstring device = L"\\\\.\\" + volumeRoot;
    HandleGuard guard;
    // FILE_FLAG_SEQUENTIAL_SCAN 提示缓存管理器做顺序预读：MFT 的访问模式
    // 正是从头到尾的顺序流，这个提示能明显减少小粒度读的等待。
    guard.h = CreateFileW(device.c_str(), GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                          FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (guard.h == INVALID_HANDLE_VALUE) {
        // 打开失败最常见的原因是未提权（ERROR_ACCESS_DENIED），其次是盘符
        // 并非 NTFS 或介质不在。附上原始 Win32 错误，方便用户自助排查。
        tree.clear();
        error = L"无法打开卷 " + volumeRoot + L"（需要管理员权限，且必须是 NTFS）: " +
                format_win32_error(GetLastError());
        progress.failed = true;
        return false;
    }
    // HandleGuard 持有句柄所有权，HandleReader 只借用；两者生命周期都在本函数
    // 内，函数返回时句柄自动关闭。
    HandleReader reader;
    reader.h = guard.h;
    return scan_mft(reader, volumeRoot, tree, progress, error);
}

}  // namespace cs
