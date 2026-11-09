// 开发/QA 工具：导出单个文件的原始 MFT 记录（需要管理员权限），
// 并并排展示我们自己的解析器从同一条记录里读出了什么。
//
//   mft_record <path-to-file>
//
// 用途：当真实卷上的大小/名称与预期不符时，用它对照「磁盘上的字节」和
// 「parse_file_record 的结论」，快速判断是解析器读错偏移，还是数据本身特殊
// （例如 $DATA 被拆到扩展记录、只有 POSIX 名称等）。

#include <windows.h>
#include <winioctl.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ntfs.hpp"

namespace {
// 小端读取辅助。用 memcpy 而不是指针强转，原因有二：
// 1. 记录内的字段偏移不保证按自然边界对齐，强转会触发未对齐访问；
// 2. 强转再解引用违反严格别名规则，优化后行为不可靠。
// 这里直接假定主机是小端（Windows 的 x86/x64/ARM64 均满足）。
uint16_t rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t rd64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        // 缺参数时打印用法并返回 2，与「运行失败返回 1」区分开。
        std::fprintf(stderr, "usage: mft_record <file> [out.txt]\n");
        return 2;
    }
    if (argc > 2) {
        // 第二个参数用于把 stdout 重定向到文件。_wfreopen_s 会替换进程级 stdout，
        // 之后所有 printf 都写进文件；这里不检查返回值——失败时 stdout 保持原样，
        // 输出落到控制台仍然可用。宽字符版本可以处理中文路径。
        FILE* f = nullptr;
        _wfreopen_s(&f, argv[2], L"wb", stdout);
    }
    const std::wstring path = argv[1];

    // 只需要读属性，所以用 FILE_READ_ATTRIBUTES 而不是 GENERIC_READ；
    // FILE_FLAG_BACKUP_SEMANTICS 是打开目录句柄的必需标志（对普通文件无害）。
    // 共享模式放开读写删，避免因为文件正被其他进程使用而打不开。
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::printf("open file failed: %lu\n", GetLastError());
        return 1;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    GetFileInformationByHandle(file, &info);
    CloseHandle(file);
    // Win32 把 64 位文件索引拆成高低两个 32 位字段，这里拼回完整 FRN：
    // 低 48 位是记录号，高 16 位是序列号（用于识别记录被回收后复用的情况）。
    const uint64_t frn =
        (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    std::printf("file     : %ls\n", path.c_str());
    // 文件大小同样由高低 32 位拼成 64 位，避免超过 4 GiB 时溢出。
    std::printf("size     : %llu bytes\n",
                (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow);
    // 掩码 0xFFFFFFFFFFFF 取出低 48 位记录号；右移 48 位得到序列号。
    std::printf("FRN      : 0x%llX (record=%llu seq=%u)\n", frn, frn & 0xFFFFFFFFFFFFull,
                static_cast<unsigned>(frn >> 48));

    // 打开卷设备。\\.\X: 形式的设备路径需要管理员权限；
    // 这里直接取路径前两个字符当盘符，因此只适用于 "C:\..." 这类盘符路径，
    // UNC 路径（\\server\share）无法用这种方式拼出卷设备名，会打开失败。
    std::wstring vol = L"\\\\.\\" + path.substr(0, 2);
    HANDLE h = CreateFileW(vol.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("open volume failed (need admin): %lu\n", GetLastError());
        return 1;
    }

    // FSCTL_GET_NTFS_FILE_RECORD：按 FRN 直接向卷驱动要原始 FILE 记录。
    // 输入必须带完整 FRN（含序列号），驱动据此拒绝已回收的记录。
    NTFS_FILE_RECORD_INPUT_BUFFER in{};
    in.FileReferenceNumber.QuadPart = static_cast<LONGLONG>(frn);
    // 输出缓冲 = 固定头 + 最大记录长度。NTFS 记录上限就是 4096 字节，
    // 因此多留 4096 一定够用；多分配一点不会影响驱动写入的长度。
    std::vector<uint8_t> out(sizeof(NTFS_FILE_RECORD_OUTPUT_BUFFER) + 4096);
    DWORD bytes = 0;
    if (!DeviceIoControl(h, FSCTL_GET_NTFS_FILE_RECORD, &in, sizeof(in), out.data(),
                         static_cast<DWORD>(out.size()), &bytes, nullptr)) {
        std::printf("FSCTL_GET_NTFS_FILE_RECORD failed: %lu\n", GetLastError());
        CloseHandle(h);
        return 1;
    }
    CloseHandle(h);

    // FileRecordBuffer 指向变长部分，FileRecordLength 才是有效长度；
    // 后续所有遍历都必须以 recLen 为界，不能假设 4096。
    auto* ob = reinterpret_cast<NTFS_FILE_RECORD_OUTPUT_BUFFER*>(out.data());
    uint8_t* rec = ob->FileRecordBuffer;
    const size_t recLen = ob->FileRecordLength;
    // 记录头：0..3 签名，+4 usaOffset（update sequence array 偏移），
    // +6 usaCount（数组元素个数，约等于记录内的扇区数 + 1）。
    std::printf("record   : %zu bytes, sig=%c%c%c%c usaOff=%u usaCount=%u\n", recLen, rec[0],
                rec[1], rec[2], rec[3], rd16(rec + 4), rd16(rec + 6));

    // 必须先撤销 fixup 才能可信地读取属性链。这里硬编码 512 字节扇区：
    // 本工具没有解析引导扇区，所以对 4Kn（4096 字节扇区）卷可能校验失败，
    // 失败时仍继续按原样导出属性，便于人工判断。
    if (!cs::ntfs::apply_fixups(rec, recLen, 512))
        std::printf("(fixup check failed; dumping raw attributes anyway)\n");
    // +22 是 FILE record 的标志位：bit0 = 使用中，bit1 = 目录。
    std::printf("flags    : 0x%04X  (inUse=%d dir=%d)\n", rd16(rec + 22),
                rd16(rec + 22) & 1, (rd16(rec + 22) >> 1) & 1);

    // +24 是记录内的已用字节数（从记录头算起，属性区结束的位置），
    // +20 是第一个属性的偏移。用 used 而不是 recLen 作为遍历上界，
    // 可以避免读到记录尾部的填充数据。
    const uint32_t used = rd32(rec + 24);
    size_t pos = rd16(rec + 20);
    std::printf("attrOffset=%zu usedSize=%u\n--- attributes ---\n", pos, used);
    // 属性按 8 字节对齐首尾相接：type(+0) / length(+4) / nonResident(+8) /
    // nameLength(+9)。遇到 0xFFFFFFFF 结束标记或长度异常即停止。
    while (pos + 16 <= used) {
        const uint32_t type = rd32(rec + pos);
        if (type == 0xFFFFFFFF) break;
        const uint32_t len = rd32(rec + pos + 4);
        // 长度小于最小属性头或用越界，说明记录已损坏，直接终止遍历。
        if (len < 16 || pos + len > used) break;
        const bool nonres = rec[pos + 8] != 0;
        const uint8_t nameLen = rec[pos + 9];
        std::printf("  type=0x%08X len=%u %s nameLen=%u", type, len,
                    nonres ? "NONRES" : "res   ", nameLen);
        if (type == 0x30 && !nonres) {
            // 常驻 $FILE_NAME：内容长度在 +16，内容偏移在 +20（都相对属性头）。
            const uint32_t cl = rd32(rec + pos + 16);
            const uint16_t co = rd16(rec + pos + 20);
            const uint8_t* fn = rec + pos + co;
            // 这里只做松散检查：确认内容长度足以覆盖 $FILE_NAME 的固定头
            // （到 +0x42，即名称起始处）。更严格的边界检查在 parse_file_record 里。
            if (static_cast<size_t>(co) + cl >= 0x42)
                // $FILE_NAME 值内：+0x28 分配大小、+0x30 逻辑大小、
                // +0x40 名称字符数、+0x41 命名空间（0=POSIX 1=Win32 2=DOS 3=两者）。
                std::printf("  [FILE_NAME alloc=%llu real=%llu ns=%u nameLen=%u]", rd64(fn + 0x28),
                            rd64(fn + 0x30), fn[0x41], fn[0x40]);
        } else if (type == 0x80 && nonres) {
            // 非驻留 $DATA：分配大小在属性头 +0x28，逻辑大小在 +0x30。
            std::printf("  [$DATA alloc=%llu real=%llu]", rd64(rec + pos + 0x28),
                        rd64(rec + pos + 0x30));
        } else if (type == 0x80 && !nonres) {
            // 常驻 $DATA：没有大小字段，内容长度（+16）就是文件大小。
            std::printf("  [$DATA resident len=%u]", rd32(rec + pos + 16));
        } else if (type == 0x20) {
            // 出现 $ATTRIBUTE_LIST 意味着属性可能被拆到扩展记录里，
            // 此时基础记录的 $DATA（和它的大小字段）可能根本不存在。
            std::printf("  [<< $ATTRIBUTE_LIST >>]");
        }
        std::printf("\n");
        pos += len;
    }

    // 最后跑一遍真正的解析器，把它的结论和上面的原始字段对照着看。
    // 记录号取 FRN 的低 48 位；扇区大小同样按 512 处理。
    cs::ntfs::FileRecord fr;
    if (cs::ntfs::parse_file_record(rec, recLen, static_cast<uint32_t>(frn & 0xFFFFFFFFFFFFull),
                                    512, fr)) {
        std::printf("--- parser ---\nparsed real=%llu alloc=%llu names=%zu dir=%d\n",
                    fr.realSize, fr.allocatedSize, fr.names.size(), fr.isDirectory);
    } else {
        std::printf("parse_file_record FAILED\n");
    }
    return 0;
}
