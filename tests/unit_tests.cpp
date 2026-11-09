// SpaceLens 核心库的单元测试。
//
// 这里刻意不使用任何测试框架：ctest 只看进程退出码，所以用一个极简 runner
// 统计「执行了多少条断言」「失败了多少条」，main() 末尾按失败数返回 0 或 1。
// 好处是没有第三方依赖，任何编译器与 CI 都能直接跑；失败时打印文件/行号和
// 断言原文，定位成本很低。
//
// 阅读顺序与文件内部的分组一致：
//   1. 构造 NTFS 内存镜像用的底层写入 helper（属性头、FILE record、boot
//      sector、run list）；
//   2. ntfs 低层解析（boot sector、run list、FILE record、fixup）；
//   3. tree 目录树聚合与分类，format 展示格式化；
//   4. MFT 扫描器：在合成卷镜像上做端到端验证；
//   5. 递归扫描器：在真实临时目录上验证（无权限路径），本文件只核对它自身的
//      已知结构，不直接与 MFT 扫描结果对比。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "format.hpp"
#include "ntfs.hpp"
#include "scan.hpp"
#include "tree.hpp"

namespace {

// 测试是单线程顺序执行的，计数器不需要原子操作；用普通 int 也避免为了日志
// 引入 <atomic>。g_checks 统计断言总数（成功 + 失败），g_failures 只统计失败。
int g_failures = 0;
int g_checks = 0;

// 全文件唯一的断言入口，语义是「记录一次检查；条件不成立就打印并计数」：
//   * 条件只求值一次，避免带副作用的表达式（如 find_child(...)）被执行两遍；
//   * 失败信息包含 __FILE__ / __LINE__ 与 #cond（表达式原文），便于直接跳转；
//   * 失败后不中断，继续跑完剩余断言，一次运行就能看到全部问题；
//   * do { ... } while (0) 保证宏展开后是单条语句，可以安全地放进 if/else。
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

// 向合成缓冲区写定宽小端整数。这里用 memcpy 而不是 *(uint32_t*)p = v：
// 字段在镜像里的位置不保证天然对齐，直接解引用属于未定义行为（x86 能跑，
// 但在严格别名/对齐检查下不可移植）。
void put16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void put64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }

// 追加一个常驻属性（resident attribute），返回它之后的下一个空闲偏移。
//
// 常驻属性头的布局（与真实 NTFS 一致，单位字节）：
//   +0x00 属性类型        +0x04 属性总长度（含头，8 字节对齐）
//   +0x08 非常驻标志(0)   +0x09 名称长度(0，属性无名)
//   +0x0A 名称偏移        +0x0C 属性标志
//   +0x0E 属性 id         +0x10 内容长度
//   +0x14 内容偏移(24)    +0x16 索引标志
//   +0x18 内容本体
// 长度向上取整到 8 字节是 NTFS 的硬性对齐要求，不这样做真实解析器可能把
// 下一条属性的头读错位；测试镜像也照此构造，才能覆盖真实路径。
size_t add_resident(uint8_t* rec, size_t pos, uint32_t type, const uint8_t* content,
                    uint32_t contentLen) {
    uint32_t length = 24 + contentLen;
    length = (length + 7u) & ~7u;
    put32(rec + pos + 0, type);
    put32(rec + pos + 4, length);
    rec[pos + 8] = 0;  // 常驻标志：内容直接在属性里，不占簇
    rec[pos + 9] = 0;  // 名称长度为 0：本文件只构造未命名属性
    put16(rec + pos + 10, 0);
    put16(rec + pos + 12, 0);
    put16(rec + pos + 14, static_cast<uint16_t>(type & 0xFFFF));  // 属性 id
    put32(rec + pos + 16, contentLen);
    put16(rec + pos + 20, 24);  // 内容紧跟 24 字节常驻头
    put16(rec + pos + 22, 0);
    std::memcpy(rec + pos + 24, content, contentLen);
    return pos + length;
}

// 追加一个非常驻的未命名 $DATA 属性（只有单个 run），返回下一个偏移。
//
// 非常驻头固定 64 字节，这里刻意使用与真实布局相同的偏移：
//   +0x20 run list 偏移   +0x28 分配大小   +0x30 实际大小   +0x38 已初始化大小
// run list 编码为 {0x11, 0x01, 0x40, 0x00}：
//   * 0x11：低半字节 1 = 长度字段 1 字节，高半字节 1 = LCN 偏移字段 1 字节；
//   * 0x01：长度 1 个簇；
//   * 0x40：相对上一个 LCN 的**有符号**增量（从 0 起算，故 LCN = 0x40）；
//   * 0x00：终止符。
// 非常驻属性才代表「文件内容真的占用了磁盘簇」，这正是容量统计要读的大小来源。
size_t add_nonresident(uint8_t* rec, size_t pos, uint64_t allocSize, uint64_t realSize) {
    const size_t runOff = 64;
    const uint8_t run[4] = {0x11, 0x01, 0x40, 0x00};  // 1 个簇 @ LCN 0x40
    uint32_t length = static_cast<uint32_t>(runOff + sizeof(run));
    length = (length + 7u) & ~7u;
    put32(rec + pos + 0, 0x80);
    put32(rec + pos + 4, length);
    rec[pos + 8] = 1;  // 非常驻标志：内容在卷上的 run 里
    rec[pos + 9] = 0;
    put16(rec + pos + 32, static_cast<uint16_t>(runOff));
    put64(rec + pos + 40, allocSize);
    put64(rec + pos + 48, realSize);
    put64(rec + pos + 56, realSize);
    std::memcpy(rec + pos + runOff, run, sizeof(run));
    return pos + length;
}

// ---------------------------------------------------------------------------
// 合成 FILE record：1024 字节、512 字节扇区、USA 计数 3。
//
// 真实 NTFS 用 Update Sequence Array（USA，更新序列数组）来发现「扇区撕裂写」：
// 每个扇区的最后 2 字节在落盘时被替换成同一个 USN，原值备份到记录头部的 USA
// 数组里；读回后只有扇区尾部等于 USN 才说明整条记录写得完整，再用备份值还原。
// 这里按两条扇区 + 1 个 USN 的布局构造（计数 3），让 apply_fixups() 走真实分支，
// 而不是在测试里绕开它。
// ---------------------------------------------------------------------------
struct RecordBuilder {
    uint8_t rec[1024];
    size_t attrPos = 56;

    // 头部固定 48 字节，属性从 +0x38(56) 开始。这里只填解析器真正依赖的字段：
    // 签名、USA 位置/数量、序列号、硬链接数、首属性偏移、记录标志；
    // LSN、base record、属性 id 计数器等留 0，解析器不应依赖它们。
    RecordBuilder(uint16_t flags, uint16_t sequence, uint16_t hardlinks) {
        std::memset(rec, 0, sizeof(rec));
        std::memcpy(rec, "FILE", 4);
        put16(rec + 4, 48);   // USA 偏移：紧跟 48 字节定长头
        put16(rec + 6, 3);    // USA 计数 = 2 个扇区 + USN 自身
        put16(rec + 16, sequence);
        put16(rec + 18, hardlinks);
        put16(rec + 20, 56);  // 第一个属性的偏移
        put16(rec + 22, flags);
    }

    // $STANDARD_INFORMATION 是纯元数据属性，**没有**文件大小字段；这里只把
    // FILE_ATTRIBUTE_ARCHIVE(0x20) 写进属性值，顺便验证解析器不会把它当大小来源。
    size_t addStandardInfo() {
        uint8_t content[48];
        std::memset(content, 0, sizeof(content));  // $STANDARD_INFORMATION 没有大小字段
        put32(content + 0x20, 0x20);               // FILE_ATTRIBUTE_ARCHIVE（归档属性）
        attrPos = add_resident(rec, attrPos, 0x10, content, sizeof(content));
        return attrPos;
    }

    // FILE_NAME 属性值的布局：父文件引用(0x00，高 16 位序列号 + 低 48 位记录号)、
    // 分配大小(0x28)、实际大小(0x30)、文件属性(0x38)、名称长度(0x40)、
    // 命名空间(0x41)、UTF-16 名称(0x42)。
    // nameSpace：0=POSIX、1=Win32、2=DOS、3=Win32&DOS，真实卷上四种都会出现，
    // 测试必须能区分，不能只认 Win32。
    size_t addFileName(uint64_t parentRef, const std::wstring& name, uint8_t nameSpace,
                       uint64_t realSize = 0, uint64_t allocSize = 0) {
        std::vector<uint8_t> content(0x42 + name.size() * 2, 0);
        put64(content.data() + 0, parentRef);
        put64(content.data() + 0x28, allocSize);  // $FILE_NAME 记录的是分配大小
        put64(content.data() + 0x30, realSize);   // $FILE_NAME 记录的是实际大小
        content[0x40] = static_cast<uint8_t>(name.size());
        content[0x41] = nameSpace;
        std::memcpy(content.data() + 0x42, name.data(), name.size() * 2);
        attrPos = add_resident(rec, attrPos, 0x30, content.data(),
                               static_cast<uint32_t>(content.size()));
        return attrPos;
    }

    size_t addNonresidentData(uint64_t allocSize, uint64_t realSize) {
        attrPos = add_nonresident(rec, attrPos, allocSize, realSize);
        return attrPos;
    }

    // 常驻 $DATA：小文件内容直接内联在记录里，大小就等于内容长度。
    size_t addResidentData(const uint8_t* data, uint32_t len) {
        attrPos = add_resident(rec, attrPos, 0x80, data, len);
        return attrPos;
    }

    // 收尾：写属性结束标记 0xFFFFFFFF，再补 used/allocated 大小，最后布置 USA。
    // 记录里两个扇区尾部（510/1022）先写成 USN，USA 数组保存「原始值」——
    // 这里用 0x0001 作为原始值，apply_fixups() 会把它还原回去。
    void finish() {
        put32(rec + attrPos + 0, 0xFFFFFFFF);
        put32(rec + attrPos + 4, 0);
        attrPos += 8;
        put32(rec + 24, static_cast<uint32_t>(attrPos));  // 已使用字节数
        put32(rec + 28, 1024);                            // 记录分配到的字节数
        put16(rec + 48, 0x0001);
        put16(rec + 50, 0x0000);
        put16(rec + 52, 0x0000);
        put16(rec + 510, 0x0001);
        put16(rec + 1022, 0x0001);
    }
};

// ---------------------------------------------------------------------------
// 端到端 MFT 扫描用的合成 NTFS 卷镜像。
//   cluster = 4096，record = 1024
//   LCN 0 = boot sector，LCN 1 = $MFT 自身的 FILE record，LCN 2 起 = $MFT 数据
// 之所以单独放一条 $MFT 记录，是因为真实卷上 $MFT 也把自己的 $DATA 记为非常驻
// 属性；扫描器必须先从记录 0 解析出 MFT 的 run list，才能知道去哪读其他记录。
// 如果测试镜像跳过这一步，扫描器里最关键的一段代码就永远走不到。
// ---------------------------------------------------------------------------

// 一个目录项（一条 FILE_NAME 属性）。parent 用「记录号 + 序列号」两个字段表达，
// 便于构造序列号不匹配的坏引用；dirFlag 控制 FILE_ATTRIBUTE_DIRECTORY 位。
// 同一个 RecSpec 放多个 NameSpec 就等于给一条记录挂多个硬链接（或 DOS 短名）。
struct NameSpec {
    uint16_t parent = 0;
    uint16_t parentSeq = 0;
    std::wstring name;
    uint8_t nameSpace = 1;  // 默认 Win32
    bool dirFlag = false;
};

// 一条 FILE record 的描述。present=false 表示该记录号在镜像里是空洞（已释放）。
//   extData             ：本记录持有未命名 $DATA extent（即属性拆分后的扩展记录）
//   hasAttrList         ：基础记录用常驻 $ATTRIBUTE_LIST 指向 dataExtRecord
//   nonResidentAttrList ：$ATTRIBUTE_LIST 本身是非常驻的，内容存在 run 里
// 这三种形态在真实卷上分别对应「大文件属性拆分」「属性列表很小」「属性列表
// 本身也放不下」，扫描器需要三条不同的取值路径。size 只对文件有意义，目录恒为 0。
struct RecSpec {
    bool present = false;
    bool isDir = false;
    bool extData = false;        // 本记录持有未命名 $DATA extent（属性拆分的扩展记录）
    bool hasAttrList = false;    // 基础记录通过 $ATTRIBUTE_LIST 指向 dataExtRecord
    bool nonResidentAttrList = false;  // $ATTRIBUTE_LIST 内容存在 run 里
    uint16_t dataExtRecord = 0;
    uint16_t seq = 0;
    uint64_t size = 0;
    std::vector<NameSpec> names;
};

std::vector<uint8_t> build_volume(const std::vector<RecSpec>& specs) {
    constexpr uint32_t kCluster = 4096;
    constexpr uint32_t kRecSize = 1024;
    constexpr uint64_t kMftRecordOffset = 1ull * kCluster;  // LCN 1 放 $MFT 记录
    constexpr uint64_t kMftDataOffset = 2ull * kCluster;    // LCN 2 起放 MFT 数据

    // $MFT 的 $DATA 大小决定扫描器枚举多少条记录，所以 totalRecords 由这里
    // 反推出来（numRecords * 1024），而不是由 boot sector 提供。
    const uint64_t numRecords = specs.size();
    const uint64_t mftReal = numRecords * kRecSize;
    const uint32_t mftClusters = static_cast<uint32_t>((mftReal + kCluster - 1) / kCluster);
    // 非常驻 $ATTRIBUTE_LIST 的内容也要落在卷上：在 MFT 数据之后单独占一个簇，
    // 这样 run list 里才有一个真实可读的 LCN。
    bool needList = false;
    for (const auto& sp : specs)
        if (sp.nonResidentAttrList) needList = true;
    const uint32_t listCluster = needList ? (2 + mftClusters) : 0;
    const size_t imgSize = static_cast<size_t>(kMftDataOffset + mftClusters * kCluster) +
                           (needList ? kCluster : 0);
    std::vector<uint8_t> img(imgSize, 0);

    // Boot sector：NTFS 的入口。解析器从这里读 OEM 标识、扇区/簇大小、
    // $MFT 起始 LCN，以及「每条 FILE record 多少字节」的编码。
    // 偏移 3 的 8 字节 OEM 标识（"NTFS    "）是判定「这是不是 NTFS 卷」的第一道关卡。
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    put16(img.data() + 11, 512);      // 每个扇区 512 字节
    img[13] = 8;                      // 每簇 8 个扇区 -> 4096 字节
    put64(img.data() + 48, 1);        // $MFT 起始 LCN
    // 簇/记录比的编码规则：正数 N 表示「N 个簇一条记录」，负数 -N 表示
    // 「2^N 字节一条记录」。-10 -> 2^10 = 1024，这是现代卷最常见的形式。
    img[64] = static_cast<uint8_t>(static_cast<int8_t>(-10));  // 每条记录 1024 字节

    // $MFT 自己的 FILE record（记录 0）：非常驻 $DATA，单个 run 覆盖所有 MFT 簇。
    // 它属于元数据，最终不会出现在目录树里，但扫描器必须先读它。
    {
        uint8_t* rec = img.data() + kMftRecordOffset;
        std::memcpy(rec, "FILE", 4);
        put16(rec + 4, 48);
        put16(rec + 6, 1);   // USA 计数 1：只有 USN，不做扇区修补
        put16(rec + 18, 1);
        put16(rec + 20, 56);
        put16(rec + 22, 0x0001);  // 使用中
        const size_t pos = 56;
        const size_t runOff = 64;
        // 长度 = mftClusters，LCN 增量 0x02（run list 从 LCN 0 起算相对位置）。
        uint8_t run[4] = {0x11, static_cast<uint8_t>(mftClusters), 0x02, 0x00};
        uint32_t alen = static_cast<uint32_t>(runOff + sizeof(run));
        alen = (alen + 7u) & ~7u;
        put32(rec + pos + 0, 0x80);
        put32(rec + pos + 4, alen);
        rec[pos + 8] = 1;  // 非常驻
        rec[pos + 9] = 0;
        put16(rec + pos + 32, static_cast<uint16_t>(runOff));
        put64(rec + pos + 40, static_cast<uint64_t>(mftClusters) * kCluster);
        put64(rec + pos + 48, mftReal);
        std::memcpy(rec + pos + runOff, run, sizeof(run));
        put32(rec + pos + alen + 0, 0xFFFFFFFF);
        put32(rec + pos + alen + 4, 0);
        put32(rec + 24, static_cast<uint32_t>(pos + alen + 8));
        put32(rec + 28, kRecSize);
    }

    // 其余 MFT 记录：specs[i] 对应记录号 i。
    for (uint64_t i = 0; i < numRecords; ++i) {
        const RecSpec& s = specs[i];
        if (!s.present) continue;  // 空洞记录：保持全 0，签名不是 FILE
        uint8_t* rec = img.data() + kMftDataOffset + i * kRecSize;
        std::memcpy(rec, "FILE", 4);
        put16(rec + 4, 48);
        put16(rec + 6, 1);   // 合成记录不做 fixup（计数 1 表示没有扇区待还原）
        put16(rec + 16, s.seq);
        put16(rec + 18, static_cast<uint16_t>(s.names.size()));  // 硬链接数
        put16(rec + 20, 56);
        // 0x0001 = 使用中，0x0002 = 目录；真实卷上目录同样置 in-use 位。
        put16(rec + 22, static_cast<uint16_t>(0x0001 | (s.isDir ? 0x0002 : 0)));
        size_t pos = 56;
        if (!s.isDir) {
            uint8_t si[48];
            std::memset(si, 0, sizeof(si));
            put32(si + 0x20, 0x20);  // FILE_ATTRIBUTE_ARCHIVE（归档属性）
            pos = add_resident(rec, pos, 0x10, si, sizeof(si));
            if (s.extData) {
                // 扩展记录：只有它真正持有未命名 $DATA extent，基础记录的
                // $FILE_NAME 大小为 0，扫描器必须靠 $ATTRIBUTE_LIST 找过来。
                pos = add_nonresident(rec, pos, s.size, s.size);
            } else if (s.hasAttrList) {
                // 常驻 $ATTRIBUTE_LIST，只有一条未命名 $DATA 表项，指向
                // dataExtRecord。表项 26 字节：类型(0)、长度(4)、最低 VCN(8)、
                // 文件引用(16，高 16 位序列号 + 低 48 位记录号)。
                uint8_t al[26];
                std::memset(al, 0, sizeof(al));
                put32(al + 0, 0x80);
                put16(al + 4, 26);
                put64(al + 8, 0);  // 最低 VCN（数据流的起点）
                put64(al + 16, (static_cast<uint64_t>(s.dataExtRecord) << 48) | s.dataExtRecord);
                pos = add_resident(rec, pos, 0x20, al, sizeof(al));
            } else if (s.nonResidentAttrList) {
                // 非常驻 $ATTRIBUTE_LIST：属性列表本身存在 listCluster 这个簇里，
                // 扫描器要先解 run list、读簇，才能拿到指向 dataExtRecord 的表项。
                const size_t runOff = 64;
                uint32_t alen = static_cast<uint32_t>((runOff + 4 + 7) & ~7u);
                put32(rec + pos + 0, 0x20);
                put32(rec + pos + 4, alen);
                rec[pos + 8] = 1;  // 非常驻
                rec[pos + 9] = 0;
                put16(rec + pos + 32, static_cast<uint16_t>(runOff));
                put64(rec + pos + 40, 26);  // 分配大小
                put64(rec + pos + 48, 26);  // 实际大小
                // 单簇 run：0x11 头 + 长度 1 + LCN 增量 = listCluster + 终止符。
                const uint8_t run[4] = {0x11, 0x01, static_cast<uint8_t>(listCluster), 0x00};
                std::memcpy(rec + pos + runOff, run, sizeof(run));
                pos += alen;
                uint8_t* dst = img.data() + static_cast<uint64_t>(listCluster) * kCluster;
                std::memset(dst, 0, 26);
                put32(dst + 0, 0x80);
                put16(dst + 4, 26);
                put64(dst + 8, 0);
                put64(dst + 16, (static_cast<uint64_t>(s.dataExtRecord) << 48) | s.dataExtRecord);
            } else if (s.size > 0) {
                // 最常见形态：未命名 $DATA 就在基础记录里。
                pos = add_nonresident(rec, pos, s.size, s.size);
            }
        }
        // 每条 FILE_NAME 属性 = 一个目录项 = 一个树节点。硬链接就是同一记录里
        // 出现多个 FILE_NAME，父目录不同；资源管理器也是这么算的。
        for (const auto& nm : s.names) {
            std::vector<uint8_t> content(0x42 + nm.name.size() * 2, 0);
            // 父引用高 16 位是序列号、低 48 位是记录号。序列号会被扫描器校验，
            // 用来避免已回收的记录被挂到错误的目录下。
            const uint64_t pref =
                (static_cast<uint64_t>(nm.parentSeq) << 48) | nm.parent;
            put64(content.data() + 0, pref);
            // 目录没有 $DATA；属性被拆分的文件，基础记录的 $FILE_NAME 大小也是 0。
            // 这与真实卷一致：这两种情况下 $FILE_NAME 不能当作兜底来源。
            const uint64_t fnSize = (s.isDir || s.hasAttrList) ? 0 : s.size;
            put64(content.data() + 0x28, fnSize);
            put64(content.data() + 0x30, fnSize);
            content[0x40] = static_cast<uint8_t>(nm.name.size());
            content[0x41] = nm.nameSpace;
            if (nm.dirFlag) put32(content.data() + 0x38, 0x10000000);  // 目录属性位
            std::memcpy(content.data() + 0x42, nm.name.data(), nm.name.size() * 2);
            pos = add_resident(rec, pos, 0x30, content.data(),
                               static_cast<uint32_t>(content.size()));
        }
        put32(rec + pos + 0, 0xFFFFFFFF);
        put32(rec + pos + 4, 0);
        pos += 8;
        put32(rec + 24, static_cast<uint32_t>(pos));
        put32(rec + 28, kRecSize);
    }
    return img;
}

// 把内存镜像伪装成卷句柄。越界读返回 false，模拟「卷被截断 / MFT 大小撒谎」，
// 让 scan_mft 的失败路径可以被测试，而不必真的去搞一块坏盘。
struct MemReader : cs::VolumeReader {
    const std::vector<uint8_t>* data = nullptr;
    bool read(uint64_t offset, void* buffer, uint32_t size) override {
        if (offset + size > data->size()) return false;
        std::memcpy(buffer, data->data() + offset, size);
        return true;
    }
};

// 任何读取都失败：模拟没有管理员权限、设备被占用或 IO 错误，
// 扫描器必须返回 false 并给出错误文本，而不是崩溃或报告成功。
struct FailingReader : cs::VolumeReader {
    bool read(uint64_t, void*, uint32_t) override { return false; }
};

// 按名称在某个父节点的子链里线性查找。子节点是单向兄弟链（arena + 索引），
// 没有哈希表，所以这里只能顺着 nextSibling 走；kInvalid 表示没找到。
uint32_t find_child(const cs::VolumeTree& t, uint32_t parent, const std::wstring& name) {
    for (uint32_t c = t.node(parent).firstChild; c != cs::VolumeTree::kInvalid;
         c = t.node(c).nextSibling) {
        if (t.nameString(c) == name) return c;
    }
    return cs::VolumeTree::kInvalid;
}

// 与 build_volume() 的预期内容一一对应的目录清单：记录号 i 的元素就是镜像里
// 记录 i 的样子。scan_mft 之后会拿这里的设计值逐项核对节点数、目录数、总大小。
// corrupt_b_sequence=true 时把 b.bin 的父序列号从 5 改成 6，用于构造
// 「父记录号存在但序列号对不上」的已回收记录场景。
std::vector<RecSpec> sample_volume_spec(bool corrupt_b_sequence) {
    std::vector<RecSpec> s(35);  // 35 条记录，其中大部分是空洞
    s[0].present = true;  // $MFT（元数据，最终必须被忽略）
    s[0].seq = 0;
    s[0].names.push_back({5, 5, L"$MFT", 1, false});

    // 记录 5 是 NTFS 根目录，以 "." 命名（真实卷上根目录确实这样挂在记录 5）。
    s[5].present = true;
    s[5].isDir = true;
    s[5].seq = 5;
    s[5].names.push_back({5, 5, L".", 1, true});

    s[16].present = true;
    s[16].isDir = true;
    s[16].seq = 16;
    s[16].names.push_back({5, 5, L"docs", 1, true});

    // 普通小文件：$DATA 与 $FILE_NAME 大小一致。
    s[17].present = true;
    s[17].seq = 17;
    s[17].size = 1000;
    s[17].names.push_back({16, 16, L"a.txt", 1, false});

    // 父目录是根；序列号可被 corrupt_b_sequence 改坏，用于验证父引用校验。
    s[18].present = true;
    s[18].seq = 18;
    s[18].size = 2000;
    s[18].names.push_back({5, static_cast<uint16_t>(corrupt_b_sequence ? 6 : 5), L"b.bin", 1,
                           false});

    // 硬链接：一条 MFT 记录、两个目录项（根和 docs 各一个），应产生两个节点。
    s[19].present = true;
    s[19].seq = 19;
    s[19].size = 500;  // 硬链接：同一记录两个名称、两个父目录
    s[19].names.push_back({5, 5, L"c.txt", 1, false});
    s[19].names.push_back({16, 16, L"c.txt", 1, false});

    // 父记录 99 不存在（悬空父目录）：节点会建出来但挂不到根上，计入孤儿。
    s[20].present = true;
    s[20].seq = 20;
    s[20].size = 700;  // 父目录悬空 -> 最终被跳过
    s[20].names.push_back({99, 99, L"orphan.bin", 1, false});

    // 只有 POSIX 命名空间（0）的目录项。曾经因为只认 Win32(1/3) 而整棵子树
    // 丢失，这里作为回归用例：POSIX 名称也必须计入。
    s[21].present = true;
    s[21].isDir = true;
    s[21].seq = 21;
    s[21].names.push_back({5, 5, L"posixdir", 0, true});

    s[22].present = true;
    s[22].seq = 22;
    s[22].size = 900;
    s[22].names.push_back({21, 21, L"p.dat", 0, false});

    // 属性拆分：基础记录 30 只有 $ATTRIBUTE_LIST，真正的 $DATA extent 在
    // 记录 31；且记录 30 的 $FILE_NAME 大小为 0（真实卷就是这样）。
    // 回归点：必须顺着 $ATTRIBUTE_LIST 找到 VCN 最低的 $DATA，而不是读 0。
    s[30].present = true;
    s[30].seq = 30;
    s[30].hasAttrList = true;
    s[30].dataExtRecord = 31;
    s[30].names.push_back({5, 5, L"big.bin", 1, false});

    s[31].present = true;
    s[31].extData = true;
    s[31].seq = 31;
    s[31].size = 1234567;  // 1.23 MB 存在扩展记录里

    // 更极端：$ATTRIBUTE_LIST 本身放不进记录，变成非常驻属性，内容在卷上的
    // 某个簇里；扫描器要先解码 run list 并读取该簇。
    s[32].present = true;
    s[32].seq = 32;
    s[32].nonResidentAttrList = true;
    s[32].dataExtRecord = 33;
    s[32].names.push_back({5, 5, L"nonres.bin", 1, false});

    s[33].present = true;
    s[33].extData = true;
    s[33].seq = 33;
    s[33].size = 222222;

    // 同一个父目录、同一文件的两个名字：一个 Win32(1)，一个 DOS(2)，且父引用
    // 的序列号不同（5 vs 6，真实卷上 DOS 名可能来自旧记录）。只应产生一个节点，
    // 并优先采用 Win32 名称。
    s[34].present = true;
    s[34].seq = 34;
    s[34].size = 300;
    s[34].names.push_back({5, 5, L"dup.bin", 1, false});
    s[34].names.push_back({5, 6, L"DUP~1.BIN", 2, false});
    return s;
}

// --------------------------------- 分组：ntfs 低层解析 ---------------------
// boot sector 解析：字段都在固定偏移上，这里逐项验证换算结果，尤其是
// 「每簇字节数 = 扇区大小 × 每簇扇区数」和「记录大小」的负指数编码。
void test_boot_sector() {
    // 512 字节足够：真实扫描会读 4096 字节凑满一页，但解析只依赖前 512 字节。
    uint8_t boot[512];
    std::memset(boot, 0, sizeof(boot));
    std::memcpy(boot + 3, "NTFS    ", 8);
    put16(boot + 11, 512);
    boot[13] = 8;
    put64(boot + 48, 0x400000);
    boot[64] = 0xF6;  // -10 -> 2^10 = 1024

    cs::ntfs::BootSector bs;
    CHECK(cs::ntfs::parse_boot_sector(boot, sizeof(boot), bs));
    CHECK(bs.bytesPerSector == 512);
    CHECK(bs.bytesPerCluster == 4096);
    CHECK(bs.bytesPerFileRecord == 1024);
    CHECK(bs.mftLcn == 0x400000);

    // cpr 的字节被解释为有符号数：0x80 = -128，取负得到 128，
    // 而 1u << 128 是未定义行为。解析器必须干净地拒绝，不能崩溃或返回假大小。
    boot[64] = 0x80;
    CHECK(!cs::ntfs::parse_boot_sector(boot, sizeof(boot), bs));
    boot[64] = 0xF6;

    // 缓冲区连固定头都不够（10 字节）时必须失败；OEM 标识不是 NTFS 时也必须失败。
    CHECK(!cs::ntfs::parse_boot_sector(boot, 10, bs));
    std::memcpy(boot + 3, "FAT32   ", 8);
    CHECK(!cs::ntfs::parse_boot_sector(boot, sizeof(boot), bs));
}

// run list 解码。关键点：每个 run 的 LCN 字段是**相对上一个 LCN 的有符号增量**，
// 不是绝对 LCN；这既压缩了体积，也解释了为什么 delta 会出现 0xFE 这种值。
void test_run_list() {
    // run0: 头 0x11(长度 1 字节/偏移 1 字节) 长度 10 LCN +0x20
    // run1: 长度 5，增量 0xFE = -2 -> LCN 0x1E（验证有符号扩展）
    // run2: 头 0x01 表示没有 LCN 字段 = 稀疏 run，长度 4
    const uint8_t data[] = {0x11, 0x0A, 0x20, 0x11, 0x05, 0xFE, 0x01, 0x04, 0x00};
    std::vector<cs::ntfs::Run> runs;
    CHECK(cs::ntfs::decode_run_list(data, sizeof(data), runs));
    CHECK(runs.size() == 3);
    CHECK(runs[0].lcn == 0x20 && runs[0].lengthClusters == 10);
    CHECK(runs[1].lcn == 0x1E && runs[1].lengthClusters == 5);
    CHECK(runs[2].sparse && runs[2].lengthClusters == 4);

    // 只有一个终止符：合法的空 run list，必须成功且产出 0 个 run。
    const uint8_t empty[] = {0x00};
    CHECK(cs::ntfs::decode_run_list(empty, sizeof(empty), runs));
    CHECK(runs.empty());

    const uint8_t bad[] = {0x11, 0x0A};  // 声明了 1 字节偏移，但后面没有数据
    CHECK(!cs::ntfs::decode_run_list(bad, sizeof(bad), runs));

    // 第二个 run 的 delta=1 会让 LCN 从 INT64_MAX 溢出（有符号加法回绕）。
    // 这类恶意/损坏的 run list 必须在解码阶段拒绝，避免后续偏移计算出负数。
    uint8_t overflow[20] = {};
    overflow[0] = 0x88;  // 8 字节长度 + 8 字节 LCN 偏移
    put64(overflow + 1, 1);
    put64(overflow + 9, 0x7FFFFFFFFFFFFFFFull);
    overflow[17] = 0x11;  // 1 字节长度 + 1 字节 LCN 偏移
    overflow[18] = 1;
    overflow[19] = 1;
    CHECK(!cs::ntfs::decode_run_list(overflow, sizeof(overflow), runs));
}

// 畸形记录边界：解析器必须先验证属性长度，再决定能读多少字段，
// 否则一条损坏记录就可能让进程越界读取甚至崩溃。
void test_malformed_record_bounds() {
    // 属性只声明 16 字节（非常驻头的公共部分），不足以容纳 +0x30 的实际大小。
    // 正确行为是「记录本身有效，但大小按 0 处理」，而不是读越界内存。
    uint8_t rec[96] = {};
    std::memcpy(rec, "FILE", 4);
    put16(rec + 20, 48);  // 第一个属性的偏移
    put16(rec + 22, 0x0001);
    put32(rec + 24, sizeof(rec));  // 已使用字节数
    put32(rec + 48, 0x80);         // $DATA
    put32(rec + 52, 16);           // 属性长度只到公共头
    rec[56] = 1;                   // 非常驻标志
    {
        cs::ntfs::FileRecord fr;
        CHECK(cs::ntfs::parse_file_record(rec, sizeof(rec), 1, 512, fr));
        CHECK(fr.valid);
        CHECK(fr.realSize == 0 && fr.allocatedSize == 0);
    }

    // 常驻属性的 contentOff/contentLen 都不能超出属性长度。这里用 0xFFFF 的
    // 偏移模拟「字段回绕/被写坏」；解析器应丢弃该属性、names 为空，仍保持
    // 记录有效，让扫描能继续跑完整卷而不是整体失败。
    std::memset(rec, 0, sizeof(rec));
    std::memcpy(rec, "FILE", 4);
    put16(rec + 20, 48);
    put16(rec + 22, 0x0001);
    put32(rec + 24, sizeof(rec));
    put32(rec + 48, 0x30);  // $FILE_NAME
    put32(rec + 52, 24);    // 常驻属性头长度
    put32(rec + 48 + 16, 0x100);   // 内容长度
    put16(rec + 48 + 20, 0xFFFF);  // 内容偏移远超属性长度
    {
        cs::ntfs::FileRecord fr;
        CHECK(cs::ntfs::parse_file_record(rec, sizeof(rec), 1, 512, fr));
        CHECK(fr.valid);
        CHECK(fr.names.empty());
    }
}

// FILE record 的正常路径：$STANDARD_INFORMATION + $FILE_NAME + 非常驻 $DATA。
// 覆盖固定头解析、fixup 还原、序列号、大小来源与名称拷贝。
// 记录号 42 只是任意值，用来确认 recordNumber 被原样带出，不参与其它逻辑。
void test_file_record() {
    RecordBuilder rb(0x0001, 7, 1);
    rb.addStandardInfo();
    rb.addFileName(5, L"hello.txt", 1, 4096, 4096);
    rb.addNonresidentData(4096, 4096);
    rb.finish();

    CHECK(cs::ntfs::apply_fixups(rb.rec, 1024, 512));
    cs::ntfs::FileRecord fr;
    CHECK(cs::ntfs::parse_file_record(rb.rec, 1024, 42, 512, fr));
    CHECK(fr.valid && fr.inUse && !fr.isDirectory);
    CHECK(fr.sequence == 7);
    // 大小取未命名 $DATA（非常驻头的分配/实际大小），不是 $FILE_NAME。
    CHECK(fr.realSize == 4096 && fr.allocatedSize == 4096);
    CHECK(fr.names.size() == 1);
    CHECK(fr.names[0].parentRef == 5);
    CHECK(std::wstring(fr.names[0].name, fr.names[0].nameLength) == L"hello.txt");
}

// 真实卷上 $FILE_NAME 里的那对大小只是「写名称时」的快照，文件被追加写入后
// 往往不再同步更新；$DATA 才是权威来源。这里故意让两者不一致以验证优先级：
// 若取 $FILE_NAME 会得到 4096，正确结果应是 $DATA 的 5000/8192。
void test_data_authoritative() {
    // $DATA 优先于 $FILE_NAME，即使后者带着过期的旧大小。
    RecordBuilder rb(0x0001, 2, 1);
    rb.addStandardInfo();
    rb.addFileName(5, L"stale.bin", 1, 4096, 4096);
    rb.addNonresidentData(8192, 5000);
    rb.finish();
    CHECK(cs::ntfs::apply_fixups(rb.rec, 1024, 512));
    cs::ntfs::FileRecord fr;
    CHECK(cs::ntfs::parse_file_record(rb.rec, 1024, 2, 512, fr));
    CHECK(fr.realSize == 5000);
    CHECK(fr.allocatedSize == 8192);
}

// 完全没有 $DATA 属性时的兜底：只有 $FILE_NAME 可读，就用它的 0x28/0x30。
// 注意这只是「没有更好信息」时的退路；属性拆分场景下 $FILE_NAME 大小为 0，
// 不能当作真实大小（那类情况由 big.bin / nonres.bin 用例覆盖）。
void test_size_filename_fallback() {
    // 没有 $DATA 属性 -> 退回使用 $FILE_NAME 里的两个大小。
    RecordBuilder rb(0x0001, 2, 1);
    rb.addStandardInfo();
    rb.addFileName(5, L"onlyname.bin", 1, 777, 777);
    rb.finish();
    CHECK(cs::ntfs::apply_fixups(rb.rec, 1024, 512));
    cs::ntfs::FileRecord fr;
    CHECK(cs::ntfs::parse_file_record(rb.rec, 1024, 2, 512, fr));
    CHECK(fr.realSize == 777);
    CHECK(fr.allocatedSize == 777);
}

// 目录记录：标志 0x0003（使用中 + 目录），没有 $DATA。NTFS 目录的大小来自
// 索引分配，不属于容量统计口径，所以解析结果必须是 0，且仍能读出名称。
void test_directory_record() {
    RecordBuilder rb(0x0003, 9, 1);
    rb.addFileName(5, L"folder", 1);
    rb.finish();
    CHECK(cs::ntfs::apply_fixups(rb.rec, 1024, 512));
    cs::ntfs::FileRecord fr;
    CHECK(cs::ntfs::parse_file_record(rb.rec, 1024, 9, 512, fr));
    CHECK(fr.isDirectory);
    CHECK(fr.realSize == 0);
    CHECK(fr.names.size() == 1);
}

// 硬链接 + 命名空间：一条记录上挂 3 个名称（两个 Win32 硬链接 + 一个 DOS 短名）。
// 解析层只负责把三个都读出来（树层再决定如何建节点），硬链接数取自记录头
// 的字段（这里也是 3），不是从名称数反推。
void test_hardlinks_and_namespaces() {
    RecordBuilder rb(0x0001, 3, 3);
    rb.addStandardInfo();
    rb.addFileName(5, L"a.txt", 1);       // Win32 名称
    rb.addFileName(9, L"b.txt", 1);       // Win32 硬链接（另一个父目录）
    rb.addFileName(5, L"ASDF~1.TXT", 2);  // DOS 短名
    rb.finish();
    CHECK(cs::ntfs::apply_fixups(rb.rec, 1024, 512));
    cs::ntfs::FileRecord fr;
    CHECK(cs::ntfs::parse_file_record(rb.rec, 1024, 100, 512, fr));
    CHECK(fr.names.size() == 3);
    CHECK(fr.hardLinkCount == 3);
}

// 常驻 $DATA 路径：小文件内容内联在记录里、且记录里没有
// $STANDARD_INFORMATION。大小应等于内容长度，hasStandardInformation 为 false。
void test_data_fallback() {
    RecordBuilder rb(0x0001, 1, 1);
    const uint8_t payload[123] = {};
    rb.addResidentData(payload, sizeof(payload));  // 记录里没有 $STANDARD_INFORMATION
    rb.addFileName(5, L"x.dat", 1);
    rb.finish();
    CHECK(cs::ntfs::apply_fixups(rb.rec, 1024, 512));
    cs::ntfs::FileRecord fr;
    CHECK(cs::ntfs::parse_file_record(rb.rec, 1024, 1, 512, fr));
    CHECK(fr.realSize == sizeof(payload));
    CHECK(!fr.hasStandardInformation);
}

// 故意把扇区尾部的 USA 标记改坏（模拟撕裂写/坏道）。apply_fixups 必须以
// false 报告不可信，扫描器随后会计入 recordsFixupFail 而不是读进垃圾数据。
void test_bad_fixup() {
    RecordBuilder rb(0x0001, 1, 1);
    rb.addStandardInfo();
    rb.finish();
    put16(rb.rec + 510, 0x9999);  // 改坏扇区尾部的 USA 标记
    CHECK(!cs::ntfs::apply_fixups(rb.rec, 1024, 512));
}

// decode_data_attribute 的输入是整条（已 fixup 的）记录：它要在属性链里找到
// 未命名的非常驻 $DATA，解析 run list，并读出分配/实际大小。这里用 10 个簇
// × 4096 = 40960 的分配大小与 40000 的实际大小，验证两个数字分别来自属性头，
// 而不是从 run 总长度推算。
void test_decode_data_runs() {
    uint8_t rec[1024];
    std::memset(rec, 0, sizeof(rec));
    std::memcpy(rec, "FILE", 4);
    put16(rec + 4, 48);
    put16(rec + 6, 1);
    put16(rec + 20, 56);
    put16(rec + 22, 0x0001);

    const size_t pos = 56;
    const size_t runOff = 64;
    const uint8_t runList[] = {0x11, 0x0A, 0x20, 0x00};  // 1 个 run：10 簇 @ LCN 0x20
    const uint32_t alen = static_cast<uint32_t>((runOff + sizeof(runList) + 7u) & ~7u);
    put32(rec + pos + 0, 0x80);
    put32(rec + pos + 4, alen);
    rec[pos + 8] = 1;
    put16(rec + pos + 32, static_cast<uint16_t>(runOff));
    put64(rec + pos + 40, 40960);
    put64(rec + pos + 48, 40000);
    std::memcpy(rec + pos + runOff, runList, sizeof(runList));
    put32(rec + pos + alen, 0xFFFFFFFF);
    put32(rec + pos + alen + 4, 0);
    put32(rec + 24, static_cast<uint32_t>(pos + alen + 8));

    CHECK(cs::ntfs::apply_fixups(rec, 1024, 512));
    std::vector<cs::ntfs::Run> runs;
    uint64_t real = 0, alloc = 0;
    CHECK(cs::ntfs::decode_data_attribute(rec, 1024, runs, real, alloc));
    CHECK(runs.size() == 1);
    CHECK(runs[0].lcn == 0x20 && runs[0].lengthClusters == 10);
    CHECK(real == 40000 && alloc == 40960);
}

// --------------------------------- 分组：目录树 ----------------------------
// 目录树聚合：自底向上把子树大小滚到父节点；排序后子链按降序，供 GUI 直接展示。
// aggregate() 同时汇总逻辑大小 totalSize 与磁盘大小 totalAlloc，这里只断言前者，
// 两者由同一段递归累加代码产出。
void test_tree_aggregate() {
    cs::VolumeTree t;
    // addNode(名称, 名称长度, 实际大小, 分配大小, 是否目录, 源记录号)
    const uint32_t root = t.addNode(L"root", 4, 0, 0, true, 0);
    const uint32_t a = t.addNode(L"a.bin", 5, 10, 10, false, 1);
    const uint32_t b = t.addNode(L"b", 1, 0, 0, true, 2);
    const uint32_t c = t.addNode(L"c.bin", 5, 5, 5, false, 3);
    t.setRoot(root);
    t.link(root, a);
    t.link(root, b);
    t.link(b, c);
    t.aggregate();
    // 根 = a(10) + b(5)，b 的后代 c(5) 只计入 b，不能重复计入根。
    CHECK(t.node(root).totalSize == 15);
    CHECK(t.node(b).totalSize == 5);
    CHECK(t.node(a).totalSize == 10);
    CHECK(t.nameString(a) == L"a.bin");
    CHECK(t.isDirectory(b));
    CHECK(!t.isDirectory(a));
    t.sortChildrenBySizeDesc();
    CHECK(t.node(root).firstChild == a);  // 10 排在 5 前面
    CHECK(t.node(b).childCount == 1);
}

// addNode 时就按扩展名定类别；目录不参与文件类型分类，统一归 Other。
void test_tree_category() {
    cs::VolumeTree t;
    const uint32_t n = t.addNode(L"clip.MP4", 8, 1, 1, false, 0);
    CHECK(t.node(n).category == static_cast<uint8_t>(cs::Category::Video));
    const uint32_t d = t.addNode(L"dir", 3, 0, 0, true, 1);
    CHECK(t.node(d).category == static_cast<uint8_t>(cs::Category::Other));
}

// 分类函数的边界：扩展名大小写不敏感；没有扩展名、以点结尾、空名称都归 Other。
// 扩展名取最后一个 '.' 之后的部分，因此 "a.b.c" 会按 "c" 分类。
void test_categorize() {
    CHECK(cs::categorize(L"movie.MP4", 9) == cs::Category::Video);
    CHECK(cs::categorize(L"photo.jpeg", 10) == cs::Category::Image);
    CHECK(cs::categorize(L"song.mp3", 8) == cs::Category::Audio);
    CHECK(cs::categorize(L"doc.pdf", 7) == cs::Category::Document);
    CHECK(cs::categorize(L"archive.7z", 10) == cs::Category::Archive);
    CHECK(cs::categorize(L"app.exe", 7) == cs::Category::Executable);
    CHECK(cs::categorize(L"main.cpp", 8) == cs::Category::Code);
    CHECK(cs::categorize(L"README", 6) == cs::Category::Other);
    // 以点结尾时扩展名为空，不能误判成某个类别。
    CHECK(cs::categorize(L"trailing.", 9) == cs::Category::Other);
    CHECK(cs::categorize(L"", 0) == cs::Category::Other);
}

// --------------------------------- 分组：格式化 ----------------------------
// 人类可读大小：B 级别用整数，KB 及以上保留两位小数。1024 是换单位的临界点，
// 1023 必须仍然是 "1023 B"；1536 用来验证四舍五入到 "1.50 KB"。
void test_human_size() {
    CHECK(cs::human_size(0) == "0 B");
    CHECK(cs::human_size(1023) == "1023 B");
    CHECK(cs::human_size(1024) == "1.00 KB");
    CHECK(cs::human_size(1536) == "1.50 KB");
    CHECK(cs::human_size(1024ull * 1024 * 1024) == "1.00 GB");
}

// --------------------------------- 分组：MFT 扫描器（合成卷镜像）-----------
// 端到端：把整个合成镜像交给 scan_mft，验证记录枚举、父子链接、名称选择、
// 属性拆分跟随、元数据过滤与统计计数全部串起来仍然正确。
void test_mft_scan_synthetic() {
    const std::vector<uint8_t> img = build_volume(sample_volume_spec(false));
    MemReader reader;
    reader.data = &img;

    cs::VolumeTree tree;
    cs::ScanProgress progress;
    std::wstring error;
    CHECK(cs::scan_mft(reader, L"X:", tree, progress, error));
    CHECK(error.empty());
    CHECK(progress.finished.load());
    CHECK(!progress.failed.load());
    // 记录总数来自 $MFT 的 $DATA 实际大小 / 记录大小 = 35，而不是别处。
    CHECK(progress.totalRecords.load() == 35);

    const uint32_t root = tree.root();
    CHECK(root != cs::VolumeTree::kInvalid);
    CHECK(tree.isDirectory(root));

    // 节点清单：root + docs + a.txt + b.bin + c.txt(root) + c.txt(docs) + orphan
    //      + posixdir + p.dat + big.bin + nonres.bin + dup.bin = 12
    // 硬链接按目录项计数：c.txt 的两个链接各建一个节点（与资源管理器一致）。
    CHECK(tree.nodeCount() == 12);
    // fileCount 统计建出的全部文件节点（含因父目录悬空而未挂上树的 orphan.bin）。
    CHECK(progress.fileCount.load() == 9);
    CHECK(progress.dirCount.load() == 3);
    CHECK(progress.orphanNodes.load() == 1);   // orphan.bin（父目录悬空）
    CHECK(progress.orphanBytes.load() == 700);
    // 父记录 99 根本不存在，因此应归类为「找不到父」，而不是序列号不匹配。
    CHECK(progress.orphanNoParent.load() == 1);
    CHECK(progress.orphanSeqMismatch.load() == 0);
    // 大小合计：b.bin 2000 + docs 子树 1500 + 根下 c.txt 500 + p.dat 900
    //      + big.bin 1234567 + nonres.bin 222222 + dup.bin 300 = 1461989
    // 注意 hard link 的两个节点各算一次大小（两个目录项都会显示 500 字节）。
    CHECK(tree.node(root).totalSize == 1461989);

    // $DATA 在扩展记录里、基础 $FILE_NAME 大小为 0：必须跟随 $ATTRIBUTE_LIST
    // 取到 1234567，而不是把文件当成 0 字节。
    CHECK(find_child(tree, root, L"big.bin") != cs::VolumeTree::kInvalid);
    CHECK(tree.node(find_child(tree, root, L"big.bin")).totalSize == 1234567);
    // 非常驻 $ATTRIBUTE_LIST -> 同样要读到扩展记录里的 222222。
    CHECK(find_child(tree, root, L"nonres.bin") != cs::VolumeTree::kInvalid);
    CHECK(tree.node(find_child(tree, root, L"nonres.bin")).totalSize == 222222);
    // 同父目录的两个名字合并为一个节点，且显示 Win32 名而不是 DOS 短名。
    CHECK(find_child(tree, root, L"dup.bin") != cs::VolumeTree::kInvalid);
    CHECK(find_child(tree, root, L"DUP~1.BIN") == cs::VolumeTree::kInvalid);

    // 只有 POSIX 命名空间的目录/文件仍必须被恢复（历史回归：曾整棵子树丢失）。
    const uint32_t posix = find_child(tree, root, L"posixdir");
    CHECK(posix != cs::VolumeTree::kInvalid);
    CHECK(tree.node(posix).totalSize == 900);
    CHECK(find_child(tree, posix, L"p.dat") != cs::VolumeTree::kInvalid);

    // docs = a.txt(1000) + c.txt(500)，子节点数 2。
    const uint32_t docs = find_child(tree, root, L"docs");
    CHECK(docs != cs::VolumeTree::kInvalid);
    CHECK(tree.isDirectory(docs));
    CHECK(tree.node(docs).totalSize == 1500);
    CHECK(tree.node(docs).childCount == 2);

    // 硬链接的两个节点指向同一条 MFT 记录（recordIndex 相同），但位于不同父目录。
    const uint32_t cb = find_child(tree, root, L"c.txt");
    const uint32_t cc = find_child(tree, docs, L"c.txt");
    CHECK(cb != cs::VolumeTree::kInvalid && cc != cs::VolumeTree::kInvalid);
    CHECK(tree.node(cb).recordIndex == 19);
    CHECK(tree.node(cc).recordIndex == 19);  // 同一条 MFT 记录、两个目录项

    // GUI 依赖「子链已按降序」直接展示，所以最大子项必须是第一个。
    // 降序排列：big.bin（1234567）应当是 root 的第一个子节点。
    CHECK(tree.nameString(tree.node(root).firstChild) == L"big.bin");

    // 记录号 < 16 的元数据（这里只有 $MFT）不能出现在目录树里，
    // 否则用户会看到一个不属于任何目录的 "$MFT"。
    CHECK(find_child(tree, root, L"$MFT") == cs::VolumeTree::kInvalid);
}

// 父引用校验：记录号相同但序列号不同，说明该 FRN 已被回收并可能复用，
// 不能把子节点挂过去，否则会把文件算到错误的目录下。
void test_mft_sequence_check() {
    const std::vector<uint8_t> img = build_volume(sample_volume_spec(true));
    MemReader reader;
    reader.data = &img;

    cs::VolumeTree tree;
    cs::ScanProgress progress;
    std::wstring error;
    CHECK(cs::scan_mft(reader, L"X:", tree, progress, error));
    // b.bin 的父序列号不匹配 -> 被丢弃：总大小比正常情况少 2000。
    CHECK(tree.node(tree.root()).totalSize == 1459989);
    CHECK(find_child(tree, tree.root(), L"b.bin") == cs::VolumeTree::kInvalid);
    CHECK(progress.orphanSeqMismatch.load() == 1);
}

// 坏镜像：签名不对 / 读取失败时，扫描必须返回 false、写入错误文本并置 failed，
// 不能返回一棵空树却报告成功——否则 GUI 会显示「磁盘是空的」。
// 两次尝试复用同一个 tree：扫描入口会先 clear()，不会留下上一次的节点。
void test_mft_bad_images() {
    cs::VolumeTree tree;
    cs::ScanProgress progress;
    std::wstring error;

    // 全 0 镜像：boot sector 没有 "NTFS" 标识（等价于把裸设备当成 NTFS 卷读）。
    std::vector<uint8_t> zeros(8192, 0);
    MemReader reader;
    reader.data = &zeros;
    CHECK(!cs::scan_mft(reader, L"X:", tree, progress, error));
    CHECK(!error.empty());
    CHECK(progress.failed.load());

    // 第一次读取就失败：模拟无管理员权限或被其他程序独占卷句柄。
    FailingReader failing;
    cs::ScanProgress progress2;
    std::wstring error2;
    CHECK(!cs::scan_mft(failing, L"X:", tree, progress2, error2));
    CHECK(!error2.empty());
}

// --------------------------------- 分组：递归扫描器 ------------------------
// 用真实文件系统做对照：在临时目录里建一棵已知结构（含空目录和三层嵌套），
// 递归扫描结果必须与手工计算的节点数/大小完全一致。这条路径不需要管理员权限，
// 也是开发期（SPACELENS_DEV_DIR）使用的扫描实现。
void test_recursive_scanner() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "spacelens_unit_test";
    std::error_code ec;
    fs::remove_all(dir, ec);  // 清掉上一次异常退出留下的残留
    fs::create_directories(dir / "sub" / "deep", ec);
    fs::create_directories(dir / "empty", ec);

    // 用二进制模式精确写入指定位数，避免文本模式把 '\n' 之类的字符改写后
    // 导致实际大小与预期不符。
    auto write_file = [](const fs::path& p, size_t bytes) {
        std::ofstream f(p, std::ios::binary);
        std::string s(bytes, 'x');
        f.write(s.data(), static_cast<std::streamsize>(s.size()));
    };
    write_file(dir / "a.bin", 1000);
    write_file(dir / "sub" / "b.bin", 2000);
    write_file(dir / "sub" / "deep" / "c.bin", 3000);

    cs::VolumeTree tree;
    cs::ScanProgress progress;
    std::wstring error;
    CHECK(cs::scan_directory_recursive(dir.wstring(), tree, progress, error));
    // 节点数：root + sub + deep + empty + 3 个文件 = 7
    // 空目录也必须有节点（否则用户看不到它占用了目录项），大小为 0。
    CHECK(tree.nodeCount() == 7);
    CHECK(progress.fileCount.load() == 3);
    CHECK(progress.dirCount.load() == 4);
    CHECK(tree.node(tree.root()).totalSize == 6000);
    CHECK(find_child(tree, tree.root(), L"empty") != cs::VolumeTree::kInvalid);
    CHECK(tree.node(find_child(tree, tree.root(), L"empty")).totalSize == 0);

    // 提前置 cancel：扫描必须报告失败并丢弃半成品树，不能伪装成成功扫描。
    // 半成品树若被交给 GUI，用户会看到一份缺失内容的「完整」结果。
    cs::VolumeTree tree2;
    cs::ScanProgress progress2;
    progress2.cancel.store(true);
    std::wstring error2;
    CHECK(!cs::scan_directory_recursive(dir.wstring(), tree2, progress2, error2));
    CHECK(!error2.empty());
    CHECK(progress2.failed.load());
    CHECK(!progress2.finished.load());
    CHECK(tree2.nodeCount() == 0);

    fs::remove_all(dir, ec);
}

// 平台杂项：枚举卷在受限环境（无权限、无卷）下也不能抛异常/崩溃；
// format_win32_error 对已知（2 = ERROR_FILE_NOT_FOUND）和未知错误码都必须
// 返回非空文本，GUI 才不会显示空提示。
void test_volume_listing_and_errors() {
    (void)cs::list_ntfs_volumes();  // 不允许崩溃
    CHECK(!cs::format_win32_error(2).empty());  // ERROR_FILE_NOT_FOUND
    CHECK(!cs::format_win32_error(0xFFFFFFFF).empty());  // 未知错误码分支
}

}  // namespace

// main 只做调度：分组顺序与文件中的定义顺序略有不同（先跑低层解析的正常/畸形
// 路径，再跑扫描器），任何一条断言失败都会让退出码变成 1，ctest 据此判失败。
// 各用例不共享状态（fixture 每次重建），所以顺序无关，分组只为可读性。
// 返回 0 表示全部通过；打印的总数便于在 CI 日志里一眼看出覆盖规模。
int main() {
    test_boot_sector();
    test_run_list();
    test_file_record();
    test_malformed_record_bounds();
    test_data_authoritative();
    test_size_filename_fallback();
    test_directory_record();
    test_hardlinks_and_namespaces();
    test_data_fallback();
    test_bad_fixup();
    test_decode_data_runs();

    test_tree_aggregate();
    test_tree_category();
    test_categorize();

    test_human_size();

    test_mft_scan_synthetic();
    test_mft_sequence_check();
    test_mft_bad_images();

    test_recursive_scanner();
    test_volume_listing_and_errors();

    if (g_failures == 0) {
        std::printf("All %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("%d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
