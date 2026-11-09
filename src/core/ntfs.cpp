#include "ntfs.hpp"

#include <cstring>
#include <limits>

// 本文件只做纯解析：输入是磁盘上原样读出的字节，输出是结构体；不打开设备、
// 不分配大块内存、也不缓存状态，因此所有函数都能用手工构造的缓冲区做单元测试。
//
// 核心立场：卷上的数据全部视为不可信输入。每个偏移都先做边界检查再读，任何
// 异常（截断的属性、越界的 run list、非法长度）都返回 false，而不是继续解析。
// 这样即使遇到损坏卷或恶意构造的镜像，也不会越界读或陷入死循环。

namespace cs::ntfs {
namespace {

// 用小端序 memcpy 读取，而不是 *reinterpret_cast<uint16_t*>(p)：
// 后者在未对齐地址上是未定义行为，并且违反严格别名规则（strict aliasing）；
// memcpy 会被编译器优化成单条 mov/load，没有任何性能代价。
// NTFS 的磁盘字节序固定为小端，代码假定的运行平台（Windows x86-64/ARM64 的
// 小端模式）与之相同，所以这里不做字节序转换。
inline uint16_t rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t rd64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

// 遍历一条 FILE record 内的属性链，直到到达结束标记 0xFFFFFFFF。
//
// 记录头布局（偏移，字节）：
//   0x00 签名 "FILE"     0x04 USA 偏移     0x06 USA 项数
//   0x14 首个属性偏移    0x18 已使用字节数
// 属性头布局（相对属性起点）：
//   +0x00 类型(4)  +0x04 本属性总长度(4)  +0x08 是否非驻留(1)  +0x09 名字长度(1)
//
// 关键点：end 取“已使用字节数”而不是记录大小。记录尾部尚未使用的区域是回收
// 记录残留的旧数据，并不是全 0；若按记录大小去扫，会解析出垃圾属性。
struct AttrIterator {
    const uint8_t* rec;
    size_t size;
    size_t pos;
    size_t end;

    // start 的合法性检查是必要的：损坏记录里的“首属性偏移”可能落在头部之内，
    // 那样第一个“属性”就会把记录头当成属性头解析。这里保守地回退到 0x30。
    AttrIterator(const uint8_t* r, size_t s) : rec(r), size(s), pos(0), end(s) {
        if (s >= 24) {
            const uint32_t used = rd32(r + 24);
            uint32_t start = rd16(r + 20);
            if (start < 48 || start >= s) start = 48;
            end = (used >= start && used <= s) ? used : s;
            pos = start;
        }
    }

    // 成功时把 attr 指向当前属性起点，type/length/nonResident 一并取出，
    // 并把游标推进到下一个属性（属性按 8 字节对齐，但 length 已包含填充）。
    // 正因为 length 含填充，直接 pos += length 就是正确的步进；这里不额外做
    // 对齐修正，否则会把正常属性跳过头。
    // 遇到结束标记、长度不足 16 字节（连属性头都放不下）或越过 end 的“长度”
    // 都立即停止：后两种情况说明记录损坏，继续走只会越界读。
    bool next(const uint8_t*& attr, uint32_t& type, uint32_t& length, bool& nonResident) {
        while (pos + 16 <= end) {
            type = rd32(rec + pos);
            if (type == kAttrEnd || type == 0) return false;
            length = rd32(rec + pos + 4);
            if (length < 16 || pos + length > end) return false;
            nonResident = rec[pos + 8] != 0;
            attr = rec + pos;
            pos += length;
            return true;
        }
        return false;
    }
};

}  // namespace

// 在 $ATTRIBUTE_LIST 的内容里找“未命名 $DATA 且 VCN 最小”的那一项，返回持有
// 该 extent 的记录文件引用（低 48 位是记录号，高 16 位是序列号）。
//
// $ATTRIBUTE_LIST 项布局（偏移，字节）：
//   +0x00 类型(4)  +0x04 项长度(2)  +0x06 名字长度(1)
//   +0x08 该项的起始 VCN(8)  +0x10 记录引用(8)  +0x18 属性 id(2)
// 未命名属性的最短项是 26 字节，所以循环条件用 p + 26 <= length。
//
// 为什么只要 VCN 最小的一项：当 $DATA 被拆分到多条记录时，只有最低 VCN 的
// extent 头部保存着整个流的 real size / allocated size，其余 extent 的值不能
// 代表文件总大小。nlen == 0 用来排除命名数据流（如 :Zone.Identifier），否则会
// 把附属流的元数据误当成文件本体。
bool find_data_extent_in_attribute_list(const uint8_t* data, uint32_t length, uint64_t& ref) {
    bool found = false;
    uint64_t bestVcn = 0;
    uint64_t bestRef = 0;
    size_t p = 0;
    while (p + 26 <= length) {
        const uint32_t t = rd32(data + p);
        const uint16_t elen = rd16(data + p + 4);
        // elen < 26 或越过缓冲区即视为列表损坏：直接停止，不再猜测后续项。
        if (elen < 26 || p + elen > length) break;
        const uint8_t nlen = data[p + 6];
        const uint64_t vcn = rd64(data + p + 8);
        const uint64_t r = rd64(data + p + 16);
        if (t == kAttrData && nlen == 0 && (!found || vcn < bestVcn)) {
            bestVcn = vcn;
            bestRef = r;
            found = true;
        }
        p += elen;
    }
    // 未找到时 ref 保持 0（调用方需配合返回值使用，不能只看 ref）。
    ref = bestRef;
    return found;
}

// 解析 NTFS 引导扇区，提取定位 $MFT 所需的几个字段。
// 偏移：0x03 文件系统签名(8)  0x0B 每扇区字节数(2)  0x0D 每簇扇区数(1)
//       0x30 $MFT 起始 LCN(8)  0x40 每 FILE record 的簇数(1，有符号)
// 全部大小都以 cluster 为单位换算，最终得到字节数（uint64_t）。
bool parse_boot_sector(const uint8_t* data, size_t size, BootSector& out) {
    out = BootSector{};
    if (size < 72) return false;
    // 签名判断放最前面：非 NTFS 卷（FAT/exFAT 等）在这里就该被拒绝，
    // 避免把它们的引导扇区字段当 NTFS 结构解读。
    if (std::memcmp(data + 3, "NTFS    ", 8) != 0) return false;

    out.bytesPerSector    = rd16(data + 11);
    out.sectorsPerCluster = data[13];
    const int8_t cpr      = static_cast<int8_t>(data[64]);
    out.mftLcn            = static_cast<int64_t>(rd64(data + 48));

    if (out.bytesPerSector == 0 || out.sectorsPerCluster == 0) return false;
    out.bytesPerCluster = static_cast<uint32_t>(out.bytesPerSector) * out.sectorsPerCluster;

    if (cpr > 0) {
        // 正数表示“每条记录占几个 cluster”，常见于 4K 簇的卷（cpr=1）。
        out.bytesPerFileRecord = static_cast<uint32_t>(cpr) * out.bytesPerCluster;
    } else if (cpr < 0) {
        // 负数表示“记录大小 = 2^(-cpr) 字节”，与簇大小无关（典型 0xF6 => 1024）。
        // cpr 是 int8_t，取负后最大可达 128；若直接按该值移位就是 UB，
        // 所以先拒绝 >= 32 的非法位数（调用方还会再把结果限制在 4096 以内）。
        const int shift = -static_cast<int>(cpr);
        if (shift >= 32) return false;
        out.bytesPerFileRecord = 1u << shift;
    } else {
        return false;  // cpr == 0 在 NTFS 中无定义，视为损坏
    }
    out.valid = true;
    return true;
}

// 解码 run list（数据分布表）。输入是一串变长项，每项结构为：
//   header(1) + 长度字段(lenBytes 字节) + 起始偏移字段(offBytes 字节)
// header 低 4 位 = 长度字段字节数，高 4 位 = 偏移字段字节数。
//
// 两个最容易搞错的点：
//  1) 偏移字段存的是“相对上一个 LCN 的有符号增量”，不是绝对 LCN，所以必须
//     累加；并且要用符号扩展把变长补码还原成 int64_t。
//  2) offBytes == 0 表示 sparse（稀疏/空洞）项：磁盘上没有对应簇，逻辑上读作
//     全 0；这种项的 lcn 无意义，这里统一填 0 并由 sparse 标志区分。
// 长度字段的单位是 cluster，不是字节。
// VCN 从 0 开始按项累加（记录流内偏移），与 lcn 的“相对增量累加”是两套不同的
// 坐标系：vcn 单调递增，lcn 可以向前也可以向后跳。
bool decode_run_list(const uint8_t* runs, size_t size, std::vector<Run>& out) {
    out.clear();
    uint64_t vcn = 0;
    int64_t lcn = 0;
    size_t i = 0;
    while (i < size) {
        const uint8_t header = runs[i++];
        if (header == 0) return true;  // 全 0 的 header 是列表结束标记
        const unsigned lenBytes = header & 0x0F;
        const unsigned offBytes = header >> 4;
        // 长度字段必须存在（lenBytes 为 0 无法表示长度）；字段宽度上限 8 字节，
        // 超过就说明表已损坏。
        if (lenBytes == 0 || lenBytes > 8 || offBytes > 8) return false;
        if (i + lenBytes + offBytes > size) return false;

        uint64_t length = 0;
        for (unsigned b = 0; b < lenBytes; ++b)
            length |= static_cast<uint64_t>(runs[i + b]) << (8 * b);
        i += lenBytes;

        const bool sparse = (offBytes == 0);
        int64_t delta = 0;
        if (!sparse) {
            uint64_t raw = 0;
            for (unsigned b = 0; b < offBytes; ++b)
                raw |= static_cast<uint64_t>(runs[i + b]) << (8 * b);
            // 变长补码的符号扩展：最高位为 1 时把高位全部填 1。
            // bits == 64 时移位会溢出，故用 bits < 64 保护（此时 raw 已是完整 int64）。
            const unsigned bits = 8 * offBytes;
            if (bits < 64 && (raw & (1ull << (bits - 1))))
                raw |= ~((1ull << bits) - 1);  // sign extend
            delta = static_cast<int64_t>(raw);
            i += offBytes;
        }

        Run r;
        r.vcn = vcn;
        r.lengthClusters = length;
        r.sparse = sparse;
        if (sparse) {
            r.lcn = 0;
        } else {
            // run list 来自磁盘，delta 可能接近 INT64_MAX/MIN。直接相加会触发
            // 有符号溢出（UB，可能被优化器推出任意结果），所以先做范围检查。
            // 另外 LCN 是物理簇号，不允许为负。
            if (delta > 0 && lcn > (std::numeric_limits<int64_t>::max)() - delta) return false;
            if (delta < 0 && lcn < (std::numeric_limits<int64_t>::min)() - delta) return false;
            lcn += delta;
            if (lcn < 0) return false;
            r.lcn = static_cast<uint64_t>(lcn);
        }
        out.push_back(r);
        vcn += length;
    }
    return true;
}

// 还原 Update Sequence Array（USA，更新序列数组）修补。
//
// NTFS 在写入时把每个扇区最后 2 个字节替换成 USN（USA 的第一项），真正的
// 原值按顺序存在 USA 里。读取时必须逐扇区核对“末尾 2 字节 == USN”，全部通过
// 才算这条记录完整（可检测掉电导致的半写）；然后写回原值。
//
// 调用约束：必须在解析任何属性、读取 $DATA 的 run list 之前执行，而且 record
// 必须可写。校验失败表示记录损坏，调用方应跳过该记录而不是继续解析。
// 注意 usaCount 通常等于 recordSize / bytesPerSector + 1（含 USN 本身），
// 这里按磁盘上的值来，不自行推导，但用 recordSize 兜住越界。
bool apply_fixups(uint8_t* record, size_t recordSize, uint32_t bytesPerSector) {
    if (recordSize < 48 || bytesPerSector == 0) return false;
    const uint16_t usaOffset = rd16(record + 4);
    const uint16_t usaCount  = rd16(record + 6);
    if (usaCount == 0) return true;  // 没有 USA 的记录（例如某些元数据）直接放行
    const size_t usaBytes = static_cast<size_t>(usaCount) * 2;
    if (static_cast<size_t>(usaOffset) + usaBytes > recordSize) return false;

    const uint16_t usn = rd16(record + usaOffset);
    // 从 i = 1 开始：USA[0] 是 USN 本身，不是要还原的值。
    for (uint16_t i = 1; i < usaCount; ++i) {
        const size_t sectorEnd = static_cast<size_t>(i) * bytesPerSector;
        // 末尾 2 字节的位置是 sectorEnd - 2；sectorEnd < 2 或越过记录都非法。
        if (sectorEnd < 2 || sectorEnd > recordSize) return false;
        const size_t pos = sectorEnd - 2;
        if (rd16(record + pos) != usn) return false;
        const uint16_t original = rd16(record + usaOffset + i * 2);
        std::memcpy(record + pos, &original, 2);
    }
    return true;
}

// 解析一条 FILE record，取出：是否在用、是否目录、序列号、硬链接数、
// $FILE_NAME 条目，以及文件大小（字节，uint64_t）。
//
// 调用约束：record 必须已经过 apply_fixups()。out.names 里的 name 指针直接
// 指向 record 缓冲区，因此 record 的生命周期必须覆盖这些指针的使用期。
// bytesPerSector 目前不参与计算（fixup 已由调用方完成），保留参数是为了让
// 签名与 apply_fixups 对称、将来需要时不必改调用点。
bool parse_file_record(const uint8_t* record, size_t recordSize,
                       uint32_t recordNumber, uint32_t bytesPerSector,
                       FileRecord& out) {
    (void)bytesPerSector;
    // 先把输出整体复位：调用方会复用同一个 FileRecord（容量得以保留），
    // 任何残留字段都可能被误当成有效值。
    out.valid = false;
    out.inUse = false;
    out.isDirectory = false;
    out.sequence = 0;
    out.hardLinkCount = 0;
    out.recordNumber = recordNumber;
    out.realSize = 0;
    out.allocatedSize = 0;
    out.hasStandardInformation = false;
    out.dataExtentRef = 0;
    out.dataExtentRefValid = false;
    out.attrListRuns.clear();
    out.hasAttrListRuns = false;
    out.attrListRealSize = 0;
    out.names.clear();

    if (recordSize < 48) return false;
    if (std::memcmp(record, "FILE", 4) != 0) return false;

    // 记录头：+0x10 序列号(2)  +0x12 硬链接数(2)  +0x16 标志(2)
    // 标志位 bit0 = 在用，bit1 = 目录。已删除记录的标志位会被清零。
    const uint16_t flags = rd16(record + 22);
    out.inUse       = (flags & 0x0001) != 0;
    out.isDirectory = (flags & 0x0002) != 0;
    // 序列号是 FRN 高 16 位，用来识别“记录号相同但已被回收再分配”的情况。
    out.sequence    = rd16(record + 16);
    out.hardLinkCount = rd16(record + 18);

    uint64_t dataReal = 0;
    uint64_t dataAlloc = 0;
    uint64_t dataBestVcn = 0;
    bool hasData = false;
    uint64_t fnReal = 0;
    uint64_t fnAlloc = 0;
    bool hasFileNameSize = false;
    uint64_t attrListDataRef = 0;
    bool attrListFound = false;

    AttrIterator it(record, recordSize);
    const uint8_t* attr = nullptr;
    uint32_t type = 0, length = 0;
    bool nonResident = false;
    while (it.next(attr, type, length, nonResident)) {
        const uint8_t attrNameLen = attr[9];
        // AttrIterator 只保证属性头有 16 字节；下面读取常驻/非驻留
        // 字段前，必须先确认属性本身足够长，否则会越过记录缓冲区。
        if (type == kAttrStandardInformation && !nonResident) {
            // $STANDARD_INFORMATION 里没有大小字段，只能记录“存在”这一事实。
            // 文件大小必须来自 $DATA 或 $FILE_NAME，不要在这里找 size。
            out.hasStandardInformation = true;
        } else if (type == kAttrFileName && !nonResident) {
            if (length < 0x18) continue;  // 常驻头：contentLen/contentOff 在 +16/+20
            const uint32_t contentLen = rd32(attr + 16);
            const uint16_t contentOff = rd16(attr + 20);
            // 用减法判断范围，避免 contentOff + contentLen 在 32 位上回绕。
            if (contentOff > length || contentLen > length - contentOff) continue;
            if (contentLen < 0x42) continue;  // $FILE_NAME 固定头到 +0x42（名字从此开始）
            const uint8_t* fn = attr + contentOff;
            const uint8_t nameLen = fn[0x40];    // 名字长度，单位是 wchar_t 个数
            const uint8_t nameSpace = fn[0x41];  // 0=POSIX 1=Win32 2=DOS 3=Win32&DOS
            // 名字区的结束位置也要校验：0x42 + nameLen*2 不得超过属性内容长度。
            if (static_cast<size_t>(0x42) + static_cast<size_t>(nameLen) * 2 <= contentLen) {
                FileNameEntry e;
                e.parentRef  = rd64(fn + 0);   // 完整 FRN：低 48 位记录号 + 高 16 位序列号
                e.name       = reinterpret_cast<const wchar_t*>(fn + 0x42);  // UTF-16LE
                e.nameLength = nameLen;
                e.nameSpace  = nameSpace;
                if (e.nameLength > 0) out.names.push_back(e);
                // $FILE_NAME 的 flags 里 0x10000000 是目录位，用作记录头标志的补充。
                if (rd32(fn + 0x38) & 0x10000000u) out.isDirectory = true;
                // 只在第一次遇到 $FILE_NAME 时取大小作为回退值：同一个文件可能有
                // 多个 $FILE_NAME（硬链接），后续条目的大小可能为 0 或指向别名，
                // 不能反复覆盖。
                if (!hasFileNameSize) {
                    fnAlloc = rd64(fn + 0x28);
                    fnReal  = rd64(fn + 0x30);
                    hasFileNameSize = true;
                }
            }
        } else if (type == kAttrAttributeList) {
            // $ATTRIBUTE_LIST 有两种形态：
            //  - 常驻：列表内容就在记录里，可直接扫描出 $DATA 所在的记录引用；
            //  - 非常驻：列表本身存放在磁盘的 runs 里，本函数只能把 runs 和长度
            //    交给调用方（扫描器会按 $MFT 分布去读出来再扫）。
            if (!nonResident) {
                if (length < 0x18) continue;  // 常驻头
                const uint32_t contentLen = rd32(attr + 16);
                const uint16_t contentOff = rd16(attr + 20);
                // 同样使用减法检查，防止 32 位加法回绕后越界读取。
                if (contentOff > length || contentLen > length - contentOff) continue;
                attrListFound = find_data_extent_in_attribute_list(attr + contentOff,
                                                                   contentLen, attrListDataRef);
            } else {
                if (length < 0x38) continue;  // 非驻留头至少需要 0x38 字节
                const uint16_t runOff = rd16(attr + 32);
                // runOff 必须落在属性头之后、属性长度之内，否则长度相减会下溢。
                if (runOff >= 0x38 && runOff < length) {
                    decode_run_list(attr + runOff, length - runOff, out.attrListRuns);
                    out.attrListRealSize = rd64(attr + 48);  // 列表真实字节数，用于限流读取
                    out.hasAttrListRuns = !out.attrListRuns.empty();
                }
            }
        } else if (type == kAttrData && attrNameLen == 0) {
            // 只有未命名的 $DATA 才是文件本体，命名数据流（备用数据流）必须忽略，
            // 否则同一文件的多个流会让大小重复累计。
            // 属性被拆分时保留 VCN 最小的那个 extent：它的头部才带有整条流的大小。
            if (nonResident) {
                if (length < 0x38) continue;  // 非驻留头：vcn/alloc/real 到 +0x30
            } else if (length < 0x14) {
                continue;  // 常驻头：value length 在 +16（4 字节）
            }
            // 非驻留：+0x10 起始 VCN；常驻属性没有 VCN，按 0 处理。
            const uint64_t vcn = nonResident ? rd64(attr + 0x10) : 0;
            if (!hasData || vcn < dataBestVcn) {
                dataBestVcn = vcn;
                // 非驻留：+0x28 allocated size，+0x30 real size；
                // 常驻：两者都取内容长度（+16），因为没有簇级分配的概念。
                dataAlloc = nonResident ? rd64(attr + 0x28) : rd32(attr + 16);
                dataReal  = nonResident ? rd64(attr + 0x30) : rd32(attr + 16);
                hasData = true;
            }
        }
    }

    out.dataExtentRef = attrListDataRef;
    out.dataExtentRefValid = attrListFound;

    // 大小优先级：未命名 $DATA > $FILE_NAME。
    // $FILE_NAME 的回退只在“基础记录里没有 $DATA”时使用，但注意：当属性被
    // $ATTRIBUTE_LIST 拆分出去时，$FILE_NAME 的大小可能是 0，此时这个回退值
    // 并不可信，调用方还需要顺着 dataExtentRef 去扩展记录里取。
    if (hasData) {
        out.realSize = dataReal;
        out.allocatedSize = dataAlloc;
    } else if (hasFileNameSize) {
        out.realSize = fnReal;
        out.allocatedSize = fnAlloc;
    }

    out.valid = true;
    return true;
}

// 找到未命名、非驻留的 $DATA 属性并解码它的 run list。
//
// 用途：定位 $MFT 自身的分布（扫描器据此按物理簇读取整张表）。小文件常驻在
// 记录里、没有 run list，会返回 false，这不是错误：本函数只服务于需要 runs 的
// 场景（$MFT、$ATTRIBUTE_LIST 内容）。
//
// 调用约束：record 必须已经过 apply_fixups()。
bool decode_data_attribute(const uint8_t* record, size_t recordSize,
                           std::vector<Run>& runs, uint64_t& realSize,
                           uint64_t& allocatedSize) {
    runs.clear();
    realSize = 0;
    allocatedSize = 0;
    if (recordSize < 48) return false;
    if (std::memcmp(record, "FILE", 4) != 0) return false;

    AttrIterator it(record, recordSize);
    const uint8_t* attr = nullptr;
    uint32_t type = 0, length = 0;
    bool nonResident = false;
    while (it.next(attr, type, length, nonResident)) {
        if (type != kAttrData) continue;
        if (attr[9] != 0) continue;  // 命名数据流不是文件本体
        if (!nonResident) continue;  // 常驻 $DATA 没有 runs
        // 非驻留属性头至少 0x38 字节，runOff/realSize/allocatedSize
        // 都位于这个范围内；长度不足时直接跳过，避免越界读。
        if (length < 0x38) continue;
        const uint16_t runOff = rd16(attr + 32);
        // runOff 落在头部内（< 0x38）或属性之外都非法；后者会让 length - runOff
        // 下溢成巨大的 size_t。
        if (runOff < 0x38 || runOff >= length) return false;
        // 注意这里的顺序：+0x30 是 real size，+0x28 是 allocated size，
        // 与 $FILE_NAME（+0x28 alloc / +0x30 real）一致，但和常驻写法不同。
        realSize      = rd64(attr + 48);
        allocatedSize = rd64(attr + 40);
        return decode_run_list(attr + runOff, length - runOff, runs);
    }
    return false;
}

}  // namespace cs::ntfs
