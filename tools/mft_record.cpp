// Dev/QA tool: dump one file's raw MFT record (needs admin) and show what our
// parser extracts from it.
//
//   mft_record <path-to-file>

#include <windows.h>
#include <winioctl.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ntfs.hpp"

namespace {
uint16_t rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t rd64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: mft_record <file> [out.txt]\n");
        return 2;
    }
    if (argc > 2) {
        FILE* f = nullptr;
        _wfreopen_s(&f, argv[2], L"wb", stdout);
    }
    const std::wstring path = argv[1];

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
    const uint64_t frn =
        (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    std::printf("file     : %ls\n", path.c_str());
    std::printf("size     : %llu bytes\n",
                (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow);
    std::printf("FRN      : 0x%llX (record=%llu seq=%u)\n", frn, frn & 0xFFFFFFFFFFFFull,
                static_cast<unsigned>(frn >> 48));

    std::wstring vol = L"\\\\.\\" + path.substr(0, 2);
    HANDLE h = CreateFileW(vol.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("open volume failed (need admin): %lu\n", GetLastError());
        return 1;
    }

    NTFS_FILE_RECORD_INPUT_BUFFER in{};
    in.FileReferenceNumber.QuadPart = static_cast<LONGLONG>(frn);
    std::vector<uint8_t> out(sizeof(NTFS_FILE_RECORD_OUTPUT_BUFFER) + 4096);
    DWORD bytes = 0;
    if (!DeviceIoControl(h, FSCTL_GET_NTFS_FILE_RECORD, &in, sizeof(in), out.data(),
                         static_cast<DWORD>(out.size()), &bytes, nullptr)) {
        std::printf("FSCTL_GET_NTFS_FILE_RECORD failed: %lu\n", GetLastError());
        CloseHandle(h);
        return 1;
    }
    CloseHandle(h);

    auto* ob = reinterpret_cast<NTFS_FILE_RECORD_OUTPUT_BUFFER*>(out.data());
    uint8_t* rec = ob->FileRecordBuffer;
    const size_t recLen = ob->FileRecordLength;
    std::printf("record   : %zu bytes, sig=%c%c%c%c usaOff=%u usaCount=%u\n", recLen, rec[0],
                rec[1], rec[2], rec[3], rd16(rec + 4), rd16(rec + 6));

    if (!cs::ntfs::apply_fixups(rec, recLen, 512))
        std::printf("(fixup check failed; dumping raw attributes anyway)\n");
    std::printf("flags    : 0x%04X  (inUse=%d dir=%d)\n", rd16(rec + 22),
                rd16(rec + 22) & 1, (rd16(rec + 22) >> 1) & 1);

    const uint32_t used = rd32(rec + 24);
    size_t pos = rd16(rec + 20);
    std::printf("attrOffset=%zu usedSize=%u\n--- attributes ---\n", pos, used);
    while (pos + 16 <= used) {
        const uint32_t type = rd32(rec + pos);
        if (type == 0xFFFFFFFF) break;
        const uint32_t len = rd32(rec + pos + 4);
        if (len < 16 || pos + len > used) break;
        const bool nonres = rec[pos + 8] != 0;
        const uint8_t nameLen = rec[pos + 9];
        std::printf("  type=0x%08X len=%u %s nameLen=%u", type, len,
                    nonres ? "NONRES" : "res   ", nameLen);
        if (type == 0x30 && !nonres) {
            const uint32_t cl = rd32(rec + pos + 16);
            const uint16_t co = rd16(rec + pos + 20);
            const uint8_t* fn = rec + pos + co;
            if (static_cast<size_t>(co) + cl >= 0x42)
                std::printf("  [FILE_NAME alloc=%llu real=%llu ns=%u nameLen=%u]", rd64(fn + 0x28),
                            rd64(fn + 0x30), fn[0x41], fn[0x40]);
        } else if (type == 0x80 && nonres) {
            std::printf("  [$DATA alloc=%llu real=%llu]", rd64(rec + pos + 0x28),
                        rd64(rec + pos + 0x30));
        } else if (type == 0x80 && !nonres) {
            std::printf("  [$DATA resident len=%u]", rd32(rec + pos + 16));
        } else if (type == 0x20) {
            std::printf("  [<< $ATTRIBUTE_LIST >>]");
        }
        std::printf("\n");
        pos += len;
    }

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
