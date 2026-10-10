// Unit tests for the compute_space core. No test framework: a tiny harness that
// counts failures and returns non-zero, so ctest can consume it directly.

#include <cmath>
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
#include "treemap.hpp"

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
    std::vector<uint8_t> img(static_cast<size_t>(kMftDataOffset + mftClusters * kCluster), 0);

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
            if (s.size > 0) pos = add_nonresident(rec, pos, s.size, s.size);
        }
        for (const auto& nm : s.names) {
            std::vector<uint8_t> content(0x42 + nm.name.size() * 2, 0);
            const uint64_t pref =
                (static_cast<uint64_t>(nm.parentSeq) << 48) | nm.parent;
            put64(content.data() + 0, pref);
            put64(content.data() + 0x28, s.isDir ? 0 : s.size);
            put64(content.data() + 0x30, s.isDir ? 0 : s.size);
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
    std::vector<RecSpec> s(21);
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

// --------------------------------- group: treemap --------------------------
void test_treemap() {
    std::vector<double> weights = {6, 6, 4, 3, 2, 2, 1};
    cs::Rect bounds{0, 0, 100, 60};
    std::vector<cs::Rect> rects;
    cs::squarified_treemap(weights, bounds, rects);
    CHECK(rects.size() == weights.size());

    double area = 0.0;
    for (const auto& r : rects) {
        CHECK(r.x >= -0.01f && r.y >= -0.01f);
        CHECK(r.x + r.w <= bounds.w + 0.5f);
        CHECK(r.y + r.h <= bounds.h + 0.5f);
        area += static_cast<double>(r.w) * r.h;
    }
    const double expected = static_cast<double>(bounds.w) * bounds.h;
    CHECK(std::abs(area - expected) / expected < 0.02);

    size_t biggest = 0;
    for (size_t i = 1; i < rects.size(); ++i)
        if (rects[i].w * rects[i].h > rects[biggest].w * rects[biggest].h) biggest = i;
    CHECK(biggest == 0);
}

void test_treemap_edges() {
    std::vector<cs::Rect> rects;
    cs::squarified_treemap({}, cs::Rect{0, 0, 100, 100}, rects);
    CHECK(rects.empty());

    cs::squarified_treemap({42.0}, cs::Rect{0, 0, 100, 50}, rects);
    CHECK(rects.size() == 1);
    CHECK(std::abs(rects[0].w * rects[0].h - 5000.0) < 1.0);

    cs::squarified_treemap({0, 0}, cs::Rect{0, 0, 100, 100}, rects);
    CHECK(rects.size() == 2);  // no crash, empty rects

    cs::squarified_treemap({5, 5}, cs::Rect{0, 0, 0, 0}, rects);  // degenerate bounds
    CHECK(rects.size() == 2);
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
    CHECK(progress.totalRecords.load() == 21);

    const uint32_t root = tree.root();
    CHECK(root != cs::VolumeTree::kInvalid);
    CHECK(tree.isDirectory(root));

    // root + docs + a.txt + b.bin + c.txt(root) + c.txt(docs) + orphan = 7
    CHECK(tree.nodeCount() == 7);
    CHECK(progress.fileCount.load() == 5);
    CHECK(progress.dirCount.load() == 2);
    CHECK(progress.orphanNodes.load() == 1);   // orphan.bin (dangling parent)
    CHECK(progress.orphanBytes.load() == 700);
    CHECK(progress.orphanNoParent.load() == 1);
    CHECK(progress.orphanSeqMismatch.load() == 0);
    // 2000 (b) + 500 (c@root) + 1500 (docs) = 4000; orphan is dangling -> excluded
    CHECK(tree.node(root).totalSize == 4000);

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

    // Sorted descending: b.bin (2000) should be the first child of root.
    CHECK(tree.nameString(tree.node(root).firstChild) == L"b.bin");

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
    // b.bin's parent sequence does not match -> dropped: 500 + 1500 = 2000
    CHECK(tree.node(tree.root()).totalSize == 2000);
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
    const fs::path dir = fs::temp_directory_path() / "compute_space_unit_test";
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

    // Cancel before starting leaves only the root.
    cs::VolumeTree tree2;
    cs::ScanProgress progress2;
    progress2.cancel.store(true);
    std::wstring error2;
    CHECK(cs::scan_directory_recursive(dir.wstring(), tree2, progress2, error2));
    CHECK(tree2.nodeCount() == 1);

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

    test_treemap();
    test_treemap_edges();

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
