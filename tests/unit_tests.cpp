// Unit tests for the SpaceLens core. No test framework: a tiny harness that
// counts failures and returns non-zero, so ctest can consume it directly.

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

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

void put16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void put64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }

// Append a resident attribute; returns the offset just past it.
size_t add_resident(uint8_t* rec, size_t pos, uint32_t type, const uint8_t* content,
                    uint32_t contentLen) {
    uint32_t length = 24 + contentLen;
    length = (length + 7u) & ~7u;
    put32(rec + pos + 0, type);
    put32(rec + pos + 4, length);
    rec[pos + 8] = 0;  // resident
    rec[pos + 9] = 0;  // no name
    put16(rec + pos + 10, 0);
    put16(rec + pos + 12, 0);
    put16(rec + pos + 14, static_cast<uint16_t>(type & 0xFFFF));
    put32(rec + pos + 16, contentLen);
    put16(rec + pos + 20, 24);
    put16(rec + pos + 22, 0);
    std::memcpy(rec + pos + 24, content, contentLen);
    return pos + length;
}

// Append a non-resident $DATA attribute (single run) and return the next offset.
size_t add_nonresident(uint8_t* rec, size_t pos, uint64_t allocSize, uint64_t realSize) {
    const size_t runOff = 64;
    const uint8_t run[4] = {0x11, 0x01, 0x40, 0x00};  // 1 cluster @ LCN 0x40
    uint32_t length = static_cast<uint32_t>(runOff + sizeof(run));
    length = (length + 7u) & ~7u;
    put32(rec + pos + 0, 0x80);
    put32(rec + pos + 4, length);
    rec[pos + 8] = 1;  // non-resident
    rec[pos + 9] = 0;
    put16(rec + pos + 32, static_cast<uint16_t>(runOff));
    put64(rec + pos + 40, allocSize);
    put64(rec + pos + 48, realSize);
    put64(rec + pos + 56, realSize);
    std::memcpy(rec + pos + runOff, run, sizeof(run));
    return pos + length;
}

// ---------------------------------------------------------------------------
// Synthetic FILE records (1024 bytes, 512-byte sectors, USA count 3).
// ---------------------------------------------------------------------------
struct RecordBuilder {
    uint8_t rec[1024];
    size_t attrPos = 56;

    RecordBuilder(uint16_t flags, uint16_t sequence, uint16_t hardlinks) {
        std::memset(rec, 0, sizeof(rec));
        std::memcpy(rec, "FILE", 4);
        put16(rec + 4, 48);   // USA offset
        put16(rec + 6, 3);    // USA count (2 sectors + usn)
        put16(rec + 16, sequence);
        put16(rec + 18, hardlinks);
        put16(rec + 20, 56);  // first attribute offset
        put16(rec + 22, flags);
    }

    size_t addStandardInfo() {
        uint8_t content[48];
        std::memset(content, 0, sizeof(content));  // $STANDARD_INFORMATION: no size fields
        put32(content + 0x20, 0x20);               // FILE_ATTRIBUTE_ARCHIVE
        attrPos = add_resident(rec, attrPos, 0x10, content, sizeof(content));
        return attrPos;
    }

    size_t addFileName(uint64_t parentRef, const std::wstring& name, uint8_t nameSpace,
                       uint64_t realSize = 0, uint64_t allocSize = 0) {
        std::vector<uint8_t> content(0x42 + name.size() * 2, 0);
        put64(content.data() + 0, parentRef);
        put64(content.data() + 0x28, allocSize);  // $FILE_NAME allocated size
        put64(content.data() + 0x30, realSize);   // $FILE_NAME real size
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

    size_t addResidentData(const uint8_t* data, uint32_t len) {
        attrPos = add_resident(rec, attrPos, 0x80, data, len);
        return attrPos;
    }

    void finish() {
        put32(rec + attrPos + 0, 0xFFFFFFFF);
        put32(rec + attrPos + 4, 0);
        attrPos += 8;
        put32(rec + 24, static_cast<uint32_t>(attrPos));  // used size
        put32(rec + 28, 1024);                            // allocated size
        put16(rec + 48, 0x0001);
        put16(rec + 50, 0x0000);
        put16(rec + 52, 0x0000);
        put16(rec + 510, 0x0001);
        put16(rec + 1022, 0x0001);
    }
};

// ---------------------------------------------------------------------------
// Synthetic NTFS volume image for end-to-end MFT scanning.
//   cluster = 4096, record = 1024
//   LCN 0 = boot, LCN 1 = $MFT record, LCN 2.. = MFT data
// ---------------------------------------------------------------------------
struct NameSpec {
    uint16_t parent = 0;
    uint16_t parentSeq = 0;
    std::wstring name;
    uint8_t nameSpace = 1;
    bool dirFlag = false;
};

struct RecSpec {
    bool present = false;
    bool isDir = false;
    bool extData = false;        // holds an unnamed $DATA extent (extension record)
    bool hasAttrList = false;    // base record points to dataExtRecord via $ATTRIBUTE_LIST
    bool nonResidentAttrList = false;  // $ATTRIBUTE_LIST content stored in a run
    uint16_t dataExtRecord = 0;
    uint16_t seq = 0;
    uint64_t size = 0;
    std::vector<NameSpec> names;
};

std::vector<uint8_t> build_volume(const std::vector<RecSpec>& specs) {
    constexpr uint32_t kCluster = 4096;
    constexpr uint32_t kRecSize = 1024;
    constexpr uint64_t kMftRecordOffset = 1ull * kCluster;
    constexpr uint64_t kMftDataOffset = 2ull * kCluster;

    const uint64_t numRecords = specs.size();
    const uint64_t mftReal = numRecords * kRecSize;
    const uint32_t mftClusters = static_cast<uint32_t>((mftReal + kCluster - 1) / kCluster);
    bool needList = false;
    for (const auto& sp : specs)
        if (sp.nonResidentAttrList) needList = true;
    const uint32_t listCluster = needList ? (2 + mftClusters) : 0;
    const size_t imgSize = static_cast<size_t>(kMftDataOffset + mftClusters * kCluster) +
                           (needList ? kCluster : 0);
    std::vector<uint8_t> img(imgSize, 0);

    // Boot sector.
    std::memcpy(img.data() + 3, "NTFS    ", 8);
    put16(img.data() + 11, 512);      // bytes per sector
    img[13] = 8;                      // sectors per cluster -> 4096
    put64(img.data() + 48, 1);        // $MFT LCN
    img[64] = static_cast<uint8_t>(static_cast<int8_t>(-10));  // 1024-byte records

    // $MFT record (non-resident $DATA with a single run).
    {
        uint8_t* rec = img.data() + kMftRecordOffset;
        std::memcpy(rec, "FILE", 4);
        put16(rec + 4, 48);
        put16(rec + 6, 1);
        put16(rec + 18, 1);
        put16(rec + 20, 56);
        put16(rec + 22, 0x0001);
        const size_t pos = 56;
        const size_t runOff = 64;
        uint8_t run[4] = {0x11, static_cast<uint8_t>(mftClusters), 0x02, 0x00};
        uint32_t alen = static_cast<uint32_t>(runOff + sizeof(run));
        alen = (alen + 7u) & ~7u;
        put32(rec + pos + 0, 0x80);
        put32(rec + pos + 4, alen);
        rec[pos + 8] = 1;  // non-resident
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

    // MFT records.
    for (uint64_t i = 0; i < numRecords; ++i) {
        const RecSpec& s = specs[i];
        if (!s.present) continue;
        uint8_t* rec = img.data() + kMftDataOffset + i * kRecSize;
        std::memcpy(rec, "FILE", 4);
        put16(rec + 4, 48);
        put16(rec + 6, 1);
        put16(rec + 16, s.seq);
        put16(rec + 18, static_cast<uint16_t>(s.names.size()));
        put16(rec + 20, 56);
        put16(rec + 22, static_cast<uint16_t>(0x0001 | (s.isDir ? 0x0002 : 0)));
        size_t pos = 56;
        if (!s.isDir) {
            uint8_t si[48];
            std::memset(si, 0, sizeof(si));
            put32(si + 0x20, 0x20);  // FILE_ATTRIBUTE_ARCHIVE
            pos = add_resident(rec, pos, 0x10, si, sizeof(si));
            if (s.extData) {
                pos = add_nonresident(rec, pos, s.size, s.size);
            } else if (s.hasAttrList) {
                // $ATTRIBUTE_LIST with one unnamed $DATA entry -> dataExtRecord.
                uint8_t al[26];
                std::memset(al, 0, sizeof(al));
                put32(al + 0, 0x80);
                put16(al + 4, 26);
                put64(al + 8, 0);  // lowest VCN
                put64(al + 16, (static_cast<uint64_t>(s.dataExtRecord) << 48) | s.dataExtRecord);
                pos = add_resident(rec, pos, 0x20, al, sizeof(al));
            } else if (s.nonResidentAttrList) {
                // Non-resident $ATTRIBUTE_LIST; its content lives in a run at
                // listCluster and points to dataExtRecord.
                const size_t runOff = 64;
                uint32_t alen = static_cast<uint32_t>((runOff + 4 + 7) & ~7u);
                put32(rec + pos + 0, 0x20);
                put32(rec + pos + 4, alen);
                rec[pos + 8] = 1;  // non-resident
                rec[pos + 9] = 0;
                put16(rec + pos + 32, static_cast<uint16_t>(runOff));
                put64(rec + pos + 40, 26);  // allocated
                put64(rec + pos + 48, 26);  // real
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
                pos = add_nonresident(rec, pos, s.size, s.size);
            }
        }
        for (const auto& nm : s.names) {
            std::vector<uint8_t> content(0x42 + nm.name.size() * 2, 0);
            const uint64_t pref =
                (static_cast<uint64_t>(nm.parentSeq) << 48) | nm.parent;
            put64(content.data() + 0, pref);
            const uint64_t fnSize = (s.isDir || s.hasAttrList) ? 0 : s.size;
            put64(content.data() + 0x28, fnSize);
            put64(content.data() + 0x30, fnSize);
            content[0x40] = static_cast<uint8_t>(nm.name.size());
            content[0x41] = nm.nameSpace;
            if (nm.dirFlag) put32(content.data() + 0x38, 0x10000000);
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

struct MemReader : cs::VolumeReader {
    const std::vector<uint8_t>* data = nullptr;
    bool read(uint64_t offset, void* buffer, uint32_t size) override {
        if (offset + size > data->size()) return false;
        std::memcpy(buffer, data->data() + offset, size);
        return true;
    }
};

struct FailingReader : cs::VolumeReader {
    bool read(uint64_t, void*, uint32_t) override { return false; }
};

uint32_t find_child(const cs::VolumeTree& t, uint32_t parent, const std::wstring& name) {
    for (uint32_t c = t.node(parent).firstChild; c != cs::VolumeTree::kInvalid;
         c = t.node(c).nextSibling) {
        if (t.nameString(c) == name) return c;
    }
    return cs::VolumeTree::kInvalid;
}

// A catalog matching build_volume()'s expected contents.
std::vector<RecSpec> sample_volume_spec(bool corrupt_b_sequence) {
    std::vector<RecSpec> s(35);
    s[0].present = true;  // $MFT (metadata, must be ignored)
    s[0].seq = 0;
    s[0].names.push_back({5, 5, L"$MFT", 1, false});

    s[5].present = true;
    s[5].isDir = true;
    s[5].seq = 5;
    s[5].names.push_back({5, 5, L".", 1, true});

    s[16].present = true;
    s[16].isDir = true;
    s[16].seq = 16;
    s[16].names.push_back({5, 5, L"docs", 1, true});

    s[17].present = true;
    s[17].seq = 17;
    s[17].size = 1000;
    s[17].names.push_back({16, 16, L"a.txt", 1, false});

    s[18].present = true;
    s[18].seq = 18;
    s[18].size = 2000;
    s[18].names.push_back({5, static_cast<uint16_t>(corrupt_b_sequence ? 6 : 5), L"b.bin", 1,
                           false});

    s[19].present = true;
    s[19].seq = 19;
    s[19].size = 500;  // hard link: two names, two parents
    s[19].names.push_back({5, 5, L"c.txt", 1, false});
    s[19].names.push_back({16, 16, L"c.txt", 1, false});

    s[20].present = true;
    s[20].seq = 20;
    s[20].size = 700;                       // dangling parent -> skipped
    s[20].names.push_back({99, 99, L"orphan.bin", 1, false});

    // POSIX-namespace-only entries must still be counted (regression: filtering
    // them out dropped whole subtrees).
    s[21].present = true;
    s[21].isDir = true;
    s[21].seq = 21;
    s[21].names.push_back({5, 5, L"posixdir", 0, true});

    s[22].present = true;
    s[22].seq = 22;
    s[22].size = 900;
    s[22].names.push_back({21, 21, L"p.dat", 0, false});

    // Large file whose $DATA lives in an extension record; base $FILE_NAME
    // size is 0 (regression: must follow $ATTRIBUTE_LIST).
    s[30].present = true;
    s[30].seq = 30;
    s[30].hasAttrList = true;
    s[30].dataExtRecord = 31;
    s[30].names.push_back({5, 5, L"big.bin", 1, false});

    s[31].present = true;
    s[31].extData = true;
    s[31].seq = 31;
    s[31].size = 1234567;  // 1.23 MB stored in the extension record

    // File whose non-resident $ATTRIBUTE_LIST points to the $DATA extent.
    s[32].present = true;
    s[32].seq = 32;
    s[32].nonResidentAttrList = true;
    s[32].dataExtRecord = 33;
    s[32].names.push_back({5, 5, L"nonres.bin", 1, false});

    s[33].present = true;
    s[33].extData = true;
    s[33].seq = 33;
    s[33].size = 222222;

    // Same file, two names for the same parent record but different sequence
    // bits (Win32 + DOS). Must produce a single node.
    s[34].present = true;
    s[34].seq = 34;
    s[34].size = 300;
    s[34].names.push_back({5, 5, L"dup.bin", 1, false});
    s[34].names.push_back({5, 6, L"DUP~1.BIN", 2, false});
    return s;
}

// --------------------------------- group: ntfs low level ------------------
void test_boot_sector() {
    uint8_t boot[512];
    std::memset(boot, 0, sizeof(boot));
    std::memcpy(boot + 3, "NTFS    ", 8);
    put16(boot + 11, 512);
    boot[13] = 8;
    put64(boot + 48, 0x400000);
    boot[64] = 0xF6;  // -10 -> 1024

    cs::ntfs::BootSector bs;
    CHECK(cs::ntfs::parse_boot_sector(boot, sizeof(boot), bs));
    CHECK(bs.bytesPerSector == 512);
    CHECK(bs.bytesPerCluster == 4096);
    CHECK(bs.bytesPerFileRecord == 1024);
    CHECK(bs.mftLcn == 0x400000);

    // 中文注释：cpr=-128 时取负得到 128，移位 1u<<128 是 UB；必须干净地拒绝。
    boot[64] = 0x80;
    CHECK(!cs::ntfs::parse_boot_sector(boot, sizeof(boot), bs));
    boot[64] = 0xF6;

    CHECK(!cs::ntfs::parse_boot_sector(boot, 10, bs));
    std::memcpy(boot + 3, "FAT32   ", 8);
    CHECK(!cs::ntfs::parse_boot_sector(boot, sizeof(boot), bs));
}

void test_run_list() {
    const uint8_t data[] = {0x11, 0x0A, 0x20, 0x11, 0x05, 0xFE, 0x01, 0x04, 0x00};
    std::vector<cs::ntfs::Run> runs;
    CHECK(cs::ntfs::decode_run_list(data, sizeof(data), runs));
    CHECK(runs.size() == 3);
    CHECK(runs[0].lcn == 0x20 && runs[0].lengthClusters == 10);
    CHECK(runs[1].lcn == 0x1E && runs[1].lengthClusters == 5);
    CHECK(runs[2].sparse && runs[2].lengthClusters == 4);

    const uint8_t empty[] = {0x00};
    CHECK(cs::ntfs::decode_run_list(empty, sizeof(empty), runs));
    CHECK(runs.empty());

    const uint8_t bad[] = {0x11, 0x0A};  // declares 1 offset byte but none follow
    CHECK(!cs::ntfs::decode_run_list(bad, sizeof(bad), runs));

    // 中文注释：第二个 run 的 delta=1 会让 LCN 从 INT64_MAX 溢出；必须拒绝。
    uint8_t overflow[20] = {};
    overflow[0] = 0x88;  // 8 字节长度 + 8 字节 LCN 偏移
    put64(overflow + 1, 1);
    put64(overflow + 9, 0x7FFFFFFFFFFFFFFFull);
    overflow[17] = 0x11;  // 1 字节长度 + 1 字节 LCN 偏移
    overflow[18] = 1;
    overflow[19] = 1;
    CHECK(!cs::ntfs::decode_run_list(overflow, sizeof(overflow), runs));
}

void test_malformed_record_bounds() {
    // 中文注释：属性只声明 16 字节（非常驻头的公共部分），不能继续读 +0x30 的大小。
    uint8_t rec[96] = {};
    std::memcpy(rec, "FILE", 4);
    put16(rec + 20, 48);  // first attribute
    put16(rec + 22, 0x0001);
    put32(rec + 24, sizeof(rec));  // used size
    put32(rec + 48, 0x80);         // $DATA
    put32(rec + 52, 16);           // attribute length
    rec[56] = 1;                   // non-resident
    {
        cs::ntfs::FileRecord fr;
        CHECK(cs::ntfs::parse_file_record(rec, sizeof(rec), 1, 512, fr));
        CHECK(fr.valid);
        CHECK(fr.realSize == 0 && fr.allocatedSize == 0);
    }

    // 中文注释：contentOff/contentLen 都不能超出属性长度；这里用超大偏移模拟回绕。
    std::memset(rec, 0, sizeof(rec));
    std::memcpy(rec, "FILE", 4);
    put16(rec + 20, 48);
    put16(rec + 22, 0x0001);
    put32(rec + 24, sizeof(rec));
    put32(rec + 48, 0x30);  // $FILE_NAME
    put32(rec + 52, 24);    // 常驻属性头长度
    put32(rec + 48 + 16, 0x100);   // contentLen
    put16(rec + 48 + 20, 0xFFFF);  // contentOff 远超属性长度
    {
        cs::ntfs::FileRecord fr;
        CHECK(cs::ntfs::parse_file_record(rec, sizeof(rec), 1, 512, fr));
        CHECK(fr.valid);
        CHECK(fr.names.empty());
    }
}

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
    CHECK(fr.realSize == 4096 && fr.allocatedSize == 4096);
    CHECK(fr.names.size() == 1);
    CHECK(fr.names[0].parentRef == 5);
    CHECK(std::wstring(fr.names[0].name, fr.names[0].nameLength) == L"hello.txt");
}

void test_data_authoritative() {
    // $DATA wins even when $FILE_NAME carries a different (stale) size.
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

void test_size_filename_fallback() {
    // No $DATA attribute -> fall back to the $FILE_NAME sizes.
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

void test_hardlinks_and_namespaces() {
    RecordBuilder rb(0x0001, 3, 3);
    rb.addStandardInfo();
    rb.addFileName(5, L"a.txt", 1);       // Win32
    rb.addFileName(9, L"b.txt", 1);       // Win32 (hard link)
    rb.addFileName(5, L"ASDF~1.TXT", 2);  // DOS short name
    rb.finish();
    CHECK(cs::ntfs::apply_fixups(rb.rec, 1024, 512));
    cs::ntfs::FileRecord fr;
    CHECK(cs::ntfs::parse_file_record(rb.rec, 1024, 100, 512, fr));
    CHECK(fr.names.size() == 3);
    CHECK(fr.hardLinkCount == 3);
}

void test_data_fallback() {
    RecordBuilder rb(0x0001, 1, 1);
    const uint8_t payload[123] = {};
    rb.addResidentData(payload, sizeof(payload));  // no $STANDARD_INFORMATION
    rb.addFileName(5, L"x.dat", 1);
    rb.finish();
    CHECK(cs::ntfs::apply_fixups(rb.rec, 1024, 512));
    cs::ntfs::FileRecord fr;
    CHECK(cs::ntfs::parse_file_record(rb.rec, 1024, 1, 512, fr));
    CHECK(fr.realSize == sizeof(payload));
    CHECK(!fr.hasStandardInformation);
}

void test_bad_fixup() {
    RecordBuilder rb(0x0001, 1, 1);
    rb.addStandardInfo();
    rb.finish();
    put16(rb.rec + 510, 0x9999);  // corrupt USA marker
    CHECK(!cs::ntfs::apply_fixups(rb.rec, 1024, 512));
}

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
    const uint8_t runList[] = {0x11, 0x0A, 0x20, 0x00};
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

// --------------------------------- group: tree -----------------------------
void test_tree_aggregate() {
    cs::VolumeTree t;
    const uint32_t root = t.addNode(L"root", 4, 0, 0, true, 0);
    const uint32_t a = t.addNode(L"a.bin", 5, 10, 10, false, 1);
    const uint32_t b = t.addNode(L"b", 1, 0, 0, true, 2);
    const uint32_t c = t.addNode(L"c.bin", 5, 5, 5, false, 3);
    t.setRoot(root);
    t.link(root, a);
    t.link(root, b);
    t.link(b, c);
    t.aggregate();
    CHECK(t.node(root).totalSize == 15);
    CHECK(t.node(b).totalSize == 5);
    CHECK(t.node(a).totalSize == 10);
    CHECK(t.nameString(a) == L"a.bin");
    CHECK(t.isDirectory(b));
    CHECK(!t.isDirectory(a));
    t.sortChildrenBySizeDesc();
    CHECK(t.node(root).firstChild == a);  // 10 before 5
    CHECK(t.node(b).childCount == 1);
}

void test_tree_category() {
    cs::VolumeTree t;
    const uint32_t n = t.addNode(L"clip.MP4", 8, 1, 1, false, 0);
    CHECK(t.node(n).category == static_cast<uint8_t>(cs::Category::Video));
    const uint32_t d = t.addNode(L"dir", 3, 0, 0, true, 1);
    CHECK(t.node(d).category == static_cast<uint8_t>(cs::Category::Other));
}

void test_categorize() {
    CHECK(cs::categorize(L"movie.MP4", 9) == cs::Category::Video);
    CHECK(cs::categorize(L"photo.jpeg", 10) == cs::Category::Image);
    CHECK(cs::categorize(L"song.mp3", 8) == cs::Category::Audio);
    CHECK(cs::categorize(L"doc.pdf", 7) == cs::Category::Document);
    CHECK(cs::categorize(L"archive.7z", 10) == cs::Category::Archive);
    CHECK(cs::categorize(L"app.exe", 7) == cs::Category::Executable);
    CHECK(cs::categorize(L"main.cpp", 8) == cs::Category::Code);
    CHECK(cs::categorize(L"README", 6) == cs::Category::Other);
    CHECK(cs::categorize(L"trailing.", 9) == cs::Category::Other);
    CHECK(cs::categorize(L"", 0) == cs::Category::Other);
}

// --------------------------------- group: format ---------------------------
void test_human_size() {
    CHECK(cs::human_size(0) == "0 B");
    CHECK(cs::human_size(1023) == "1023 B");
    CHECK(cs::human_size(1024) == "1.00 KB");
    CHECK(cs::human_size(1536) == "1.50 KB");
    CHECK(cs::human_size(1024ull * 1024 * 1024) == "1.00 GB");
}

// --------------------------------- group: MFT scanner (synthetic volume) ---
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
    CHECK(progress.totalRecords.load() == 35);

    const uint32_t root = tree.root();
    CHECK(root != cs::VolumeTree::kInvalid);
    CHECK(tree.isDirectory(root));

    // root + docs + a.txt + b.bin + c.txt(root) + c.txt(docs) + orphan
    //      + posixdir + p.dat + big.bin + nonres.bin + dup.bin = 12
    CHECK(tree.nodeCount() == 12);
    CHECK(progress.fileCount.load() == 9);
    CHECK(progress.dirCount.load() == 3);
    CHECK(progress.orphanNodes.load() == 1);   // orphan.bin (dangling parent)
    CHECK(progress.orphanBytes.load() == 700);
    CHECK(progress.orphanNoParent.load() == 1);
    CHECK(progress.orphanSeqMismatch.load() == 0);
    // 2000 + 500 + 1500 + 900 + 1234567 + 222222 + 300 = 1461989
    CHECK(tree.node(root).totalSize == 1461989);

    // File whose $DATA is in an extension record (base $FILE_NAME size = 0).
    CHECK(find_child(tree, root, L"big.bin") != cs::VolumeTree::kInvalid);
    CHECK(tree.node(find_child(tree, root, L"big.bin")).totalSize == 1234567);
    // Non-resident $ATTRIBUTE_LIST -> extension record size.
    CHECK(find_child(tree, root, L"nonres.bin") != cs::VolumeTree::kInvalid);
    CHECK(tree.node(find_child(tree, root, L"nonres.bin")).totalSize == 222222);
    // Two names for one parent => one node, named with the Win32 name.
    CHECK(find_child(tree, root, L"dup.bin") != cs::VolumeTree::kInvalid);
    CHECK(find_child(tree, root, L"DUP~1.BIN") == cs::VolumeTree::kInvalid);

    // POSIX-namespace-only directory and file are recovered.
    const uint32_t posix = find_child(tree, root, L"posixdir");
    CHECK(posix != cs::VolumeTree::kInvalid);
    CHECK(tree.node(posix).totalSize == 900);
    CHECK(find_child(tree, posix, L"p.dat") != cs::VolumeTree::kInvalid);

    const uint32_t docs = find_child(tree, root, L"docs");
    CHECK(docs != cs::VolumeTree::kInvalid);
    CHECK(tree.isDirectory(docs));
    CHECK(tree.node(docs).totalSize == 1500);
    CHECK(tree.node(docs).childCount == 2);

    const uint32_t cb = find_child(tree, root, L"c.txt");
    const uint32_t cc = find_child(tree, docs, L"c.txt");
    CHECK(cb != cs::VolumeTree::kInvalid && cc != cs::VolumeTree::kInvalid);
    CHECK(tree.node(cb).recordIndex == 19);
    CHECK(tree.node(cc).recordIndex == 19);  // same MFT record, two entries

    // Sorted descending: big.bin (1234567) should be the first child of root.
    CHECK(tree.nameString(tree.node(root).firstChild) == L"big.bin");

    // Metadata ($MFT, record 0) must not appear.
    CHECK(find_child(tree, root, L"$MFT") == cs::VolumeTree::kInvalid);
}

void test_mft_sequence_check() {
    const std::vector<uint8_t> img = build_volume(sample_volume_spec(true));
    MemReader reader;
    reader.data = &img;

    cs::VolumeTree tree;
    cs::ScanProgress progress;
    std::wstring error;
    CHECK(cs::scan_mft(reader, L"X:", tree, progress, error));
    // b.bin's parent sequence does not match -> dropped: 500+1500+900+1234567+222222+300
    CHECK(tree.node(tree.root()).totalSize == 1459989);
    CHECK(find_child(tree, tree.root(), L"b.bin") == cs::VolumeTree::kInvalid);
    CHECK(progress.orphanSeqMismatch.load() == 1);
}

void test_mft_bad_images() {
    cs::VolumeTree tree;
    cs::ScanProgress progress;
    std::wstring error;

    std::vector<uint8_t> zeros(8192, 0);
    MemReader reader;
    reader.data = &zeros;
    CHECK(!cs::scan_mft(reader, L"X:", tree, progress, error));
    CHECK(!error.empty());
    CHECK(progress.failed.load());

    FailingReader failing;
    cs::ScanProgress progress2;
    std::wstring error2;
    CHECK(!cs::scan_mft(failing, L"X:", tree, progress2, error2));
    CHECK(!error2.empty());
}

// --------------------------------- group: recursive scanner ----------------
void test_recursive_scanner() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "spacelens_unit_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "sub" / "deep", ec);
    fs::create_directories(dir / "empty", ec);

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
    // root + sub + deep + empty + 3 files = 7
    CHECK(tree.nodeCount() == 7);
    CHECK(progress.fileCount.load() == 3);
    CHECK(progress.dirCount.load() == 4);
    CHECK(tree.node(tree.root()).totalSize == 6000);
    CHECK(find_child(tree, tree.root(), L"empty") != cs::VolumeTree::kInvalid);
    CHECK(tree.node(find_child(tree, tree.root(), L"empty")).totalSize == 0);

    // 中文注释：取消必须报告失败并丢弃半成品树，不能伪装成成功扫描。
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

void test_volume_listing_and_errors() {
    (void)cs::list_ntfs_volumes();  // must not crash
    CHECK(!cs::format_win32_error(2).empty());  // ERROR_FILE_NOT_FOUND
    CHECK(!cs::format_win32_error(0xFFFFFFFF).empty());  // unknown code path
}

}  // namespace

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
