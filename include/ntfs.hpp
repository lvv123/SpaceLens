#pragma once

// NTFS 磁盘结构的底层定义与纯解析函数。
//
// 本文件只做「字节缓冲 -> 结构体」的翻译，不做任何 I/O、不持有句柄、不分配
// 卷资源，所以每个函数都可以用测试里手工构造的缓冲区直接验证
// （见 tests/unit_tests.cpp 的 build_volume）。
//
// 重要前提：这里读到的所有字节都来自磁盘，属于不可信输入。每个解析函数都
// 必须自己检查长度和偏移，绝不能假定属性头、run list 或名称字段是完整的。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cs::ntfs {

// FILE record 中属性类型码（NTFS 磁盘格式固定值，不能随意调整）。
// 我们只关心这四种属性，末尾另有一个结束标记。
inline constexpr uint32_t kAttrStandardInformation = 0x10;
inline constexpr uint32_t kAttrAttributeList       = 0x20;
inline constexpr uint32_t kAttrFileName            = 0x30;
inline constexpr uint32_t kAttrData                = 0x80;
// 属性链的结束标记；相邻属性按 `length` 首尾相接，遇到它即停止遍历。
inline constexpr uint32_t kAttrEnd                 = 0xFFFFFFFF;

// 从 NTFS 卷引导扇区中解析出的、定位和度量 $MFT 所需的字段。
//
// 单位说明（极易混淆）：
// - bytesPerSector / bytesPerCluster 是字节数，sectorsPerCluster 是扇区个数；
// - mftLcn 是 $MFT 起始处的 LCN（逻辑簇号），不是字节偏移，
//   使用前必须乘 bytesPerCluster 才能当文件偏移；
// - bytesPerFileRecord 是单条 FILE record 的字节数（通常 1024）。
struct BootSector {
    uint16_t bytesPerSector     = 0;
    uint8_t  sectorsPerCluster  = 0;
    uint32_t bytesPerCluster    = 0;
    uint32_t bytesPerFileRecord = 0;
    int64_t  mftLcn             = 0;
    bool     valid              = false;   // parse_boot_sector 成功后才为 true
};

// 校验并解析引导扇区。`data` 至少要 72 字节，且偏移 3 处必须是 "NTFS    "。
// 注意：本函数一进来就把 out 重置为全零，因此失败时 out.valid 一定是 false，
// 调用方应直接放弃整个卷的扫描，不要读 out 里的其余字段。
bool parse_boot_sector(const uint8_t* data, size_t size, BootSector& out);

// NTFS 数据流的一段物理区段（run list 的一项）。
//
// 三个长度/位置字段的单位都是「簇」，不是字节：换算成字节要乘
// bytesPerCluster，漏乘会得到小几个数量级的结果。
struct Run {
    uint64_t vcn           = 0;   // 本区段在数据流内的起始 VCN
    uint64_t lcn           = 0;   // 卷上的起始簇号；仅 !sparse 时有意义
    uint64_t lengthClusters = 0;
    bool     sparse        = false;   // 稀疏区段不占磁盘，读取时应补零
};

// 解码 run list 字节流（属性里的 mapping pairs）。
// 每项首字节的低 4 位是长度字段宽度、高 4 位是偏移字段宽度；偏移是相对上一个
// LCN 的有符号增量，因此必须按顺序累加，不能并行解析。返回 false 表示数据
// 损坏（越界、字段过宽或累加得到负的 LCN），此时 out 内容不可用。
bool decode_run_list(const uint8_t* runs, size_t size, std::vector<Run>& out);

// 一个 $FILE_NAME 属性的值（同一文件可能有多条，硬链接即多父目录）。
struct FileNameEntry {
    uint64_t       parentRef  = 0;   // 父目录完整 FRN：低 48 位记录号 + 高 16 位序列号
    const wchar_t* name       = nullptr;  // 指向被解析记录缓冲区，不拥有内存
    uint16_t       nameLength = 0;       // 以 wchar_t 为单位，不是字节数
    uint8_t        nameSpace  = 0;       // 0=POSIX 1=Win32 2=DOS 3=Win32&DOS
};

// 单条 FILE record 的解析结果（只保留上层建树需要的字段）。
struct FileRecord {
    bool     valid         = false;  // 签名 "FILE" 正确且边界检查通过
    bool     inUse         = false;  // flags bit0：记录是否仍在使用
    bool     isDirectory   = false;  // flags bit1，或 $FILE_NAME 的目录位
    uint16_t sequence      = 0;      // 记录序列号，用于校验父子链接是否过期
    uint16_t hardLinkCount = 0;      // 硬链接计数，仅用于诊断
    uint32_t recordNumber  = 0;      // 在 $MFT 中的下标，由调用方传入（记录内没有）
    // 逻辑大小（未命名 $DATA；常驻时取内容长度）。回退到 $FILE_NAME。
    // 注意 $STANDARD_INFORMATION 没有大小字段，不要从它读。
    uint64_t realSize      = 0;
    // 磁盘占用大小（与 realSize 同源），稀疏/压缩文件会明显小于 realSize。
    uint64_t allocatedSize = 0;
    bool     hasStandardInformation = false;   // 仅诊断用：记录里是否有 $STANDARD_INFORMATION
    // 大文件/碎片文件的属性会被拆到扩展记录中：此时基础记录里没有 $DATA，
    // 其 $FILE_NAME 大小也是 0。下面这个 FRN 指向存放实际 $DATA 的记录
    // （低 48 位记录号，高 16 位序列号）；valid 为 false 时不可使用。
    uint64_t dataExtentRef = 0;
    bool     dataExtentRefValid = false;
    // 非驻留的 $ATTRIBUTE_LIST 把内容放在 runs 里：调用方必须先按 runs 把
    // 属性列表读出来，才能在里面找到 $DATA 所在的扩展记录。
    bool     hasAttrListRuns = false;
    uint64_t attrListRealSize = 0;   // 属性列表逻辑大小，用于限制读取上限
    std::vector<Run> attrListRuns;
    // 跨多次 parse 调用复用（容量保留，避免每条记录都重新分配）。
    // 其中的 name 指针指向调用方传入的 record 缓冲区。
    std::vector<FileNameEntry> names;
};

// 在 $ATTRIBUTE_LIST 的内容（不是 FILE record！）中找出 VCN 最低的未命名
// $DATA 项，并把持有该区段的记录 FRN 写入 ref。
// 之所以取 VCN 最低的一项：真实大小/分配大小只写在第一个区段（VCN=0）的
// 属性头里，其余区段的大小字段为 0。
// 返回 false 表示列表里没有未命名 $DATA，ref 保持原值。
bool find_data_extent_in_attribute_list(const uint8_t* data, uint32_t length, uint64_t& ref);

// 原地撤销 update-sequence-array（fixup）保护。
//
// NTFS 在写入每个扇区末尾时，会把最后 2 字节替换成 USN，原值保存在
// usaOffset 指向的数组里。不先恢复就直接解析，属性长度和偏移会全是错的。
// 因此凡是解析 record 或其 $DATA run list 之前，都必须先调用本函数。
// `record` 必须可写；越界或 USN 不匹配（多半是撕裂写）返回 false。
bool apply_fixups(uint8_t* record, size_t recordSize, uint32_t bytesPerSector);

// 解析一条 FILE record。前置条件：`record` 已经过 apply_fixups 处理。
// recordNumber 由调用方提供（记录本身不存自己的编号）。
// 注意 out.names 里的 name 指针别名 `record`，因此 record 必须在名字使用
// 期间保持有效；不要传入临时缓冲后立刻释放。
bool parse_file_record(const uint8_t* record, size_t recordSize,
                       uint32_t recordNumber, uint32_t bytesPerSector,
                       FileRecord& out);

// 找到未命名的、非驻留的 $DATA 属性并解码其 run list，同时输出逻辑大小与
// 分配大小。`record` 必须已经过 fixup 修正。
// 返回 false 的常见情况：常驻文件（没有 runs）、只有命名数据流、记录损坏。
bool decode_data_attribute(const uint8_t* record, size_t recordSize,
                           std::vector<Run>& runs, uint64_t& realSize,
                           uint64_t& allocatedSize);

}  // namespace cs::ntfs
