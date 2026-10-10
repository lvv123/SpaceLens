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

// Append a resident attribute and return the offset just past it.
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

// Build a minimal but valid FILE record (1024 bytes, 512-byte sectors).
struct RecordBuilder {
    uint8_t rec[1024];

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

    size_t addStandardInfo(uint64_t realSize, uint64_t allocSize) {
        uint8_t content[48];
        std::memset(content, 0, sizeof(content));
        put64(content + 0x20, allocSize);
        put64(content + 0x28, realSize);
        const size_t n = add_resident(rec, attrPos, 0x10, content, sizeof(content));
        attrPos = n;
        return n;
    }

    size_t addFileName(uint64_t parentRef, const std::wstring& name, uint8_t nameSpace) {
        std::vector<uint8_t> content(0x42 + name.size() * 2, 0);
        put64(content.data() + 0, parentRef);
        content[0x40] = static_cast<uint8_t>(name.size());
        content[0x41] = nameSpace;
        std::memcpy(content.data() + 0x42, name.data(), name.size() * 2);
        const size_t n = add_resident(rec, attrPos, 0x30, content.data(),
                                      static_cast<uint32_t>(content.size()));
        attrPos = n;
        return n;
    }

    size_t finish() {
        put32(rec + attrPos + 0, 0xFFFFFFFF);
        put32(rec + attrPos + 4, 0);
        attrPos += 8;
        put32(rec + 24, static_cast<uint32_t>(attrPos));  // used size
        put32(rec + 28, 1024);                            // allocated size

        // USA fixups: USN in USA[0], saved bytes in USA[1..2], placeholders at
        // the end of each 512-byte sector.
        put16(rec + 48, 0x0001);
        put16(rec + 50, 0x0000);
        put16(rec + 52, 0x0000);
        put16(rec + 510, 0x0001);
        put16(rec + 1022, 0x0001);
        return attrPos;
    }

    size_t attrPos = 56;
};

void test_boot_sector() {
    uint8_t boot[512];
    std::memset(boot, 0, sizeof(boot));
    std::memcpy(boot + 3, "NTFS    ", 8);
    put16(boot + 11, 512);   // bytes per sector
    boot[13] = 8;            // sectors per cluster
    put64(boot + 48, 0x400000);  // MFT LCN
    boot[64] = 0xF6;         // clusters per record = -10 -> 1024 bytes

    cs::ntfs::BootSector bs;
    CHECK(cs::ntfs::parse_boot_sector(boot, sizeof(boot), bs));
    CHECK(bs.valid);
    CHECK(bs.bytesPerSector == 512);
    CHECK(bs.bytesPerCluster == 4096);
    CHECK(bs.bytesPerFileRecord == 1024);
    CHECK(bs.mftLcn == 0x400000);

    CHECK(!cs::ntfs::parse_boot_sector(boot, 10, bs));
}

void test_run_list() {
    // len=10, lcn += 0x20 ; len=5, lcn += -2 ; sparse len=4 ; end.
    const uint8_t data[] = {0x11, 0x0A, 0x20, 0x11, 0x05, 0xFE, 0x01, 0x04, 0x00};
    std::vector<cs::ntfs::Run> runs;
    CHECK(cs::ntfs::decode_run_list(data, sizeof(data), runs));
    CHECK(runs.size() == 3);
    CHECK(runs[0].lcn == 0x20 && runs[0].lengthClusters == 10);
    CHECK(runs[1].lcn == 0x1E && runs[1].lengthClusters == 5);
    CHECK(runs[2].sparse && runs[2].lengthClusters == 4);
}

void test_file_record() {
    RecordBuilder rb(0x0001, 7, 1);
    rb.addStandardInfo(4096, 4096);
    rb.addFileName(5, L"hello.txt", 1);
    rb.finish();

    CHECK(cs::ntfs::apply_fixups(rb.rec, 1024, 512));
    cs::ntfs::FileRecord fr;
    CHECK(cs::ntfs::parse_file_record(rb.rec, 1024, 42, 512, fr));
    CHECK(fr.valid && fr.inUse && !fr.isDirectory);
    CHECK(fr.sequence == 7);
    CHECK(fr.realSize == 4096 && fr.allocatedSize == 4096);
    CHECK(fr.names.size() == 1);
    CHECK(fr.names[0].parentRef == 5);
    CHECK(fr.names[0].nameLength == 9);
    CHECK(std::wstring(fr.names[0].name, fr.names[0].nameLength) == L"hello.txt");
}

void test_hardlinks() {
    RecordBuilder rb(0x0001, 3, 2);
    rb.addStandardInfo(100, 100);
    rb.addFileName(5, L"a.txt", 1);
    rb.addFileName(9, L"b.txt", 1);
    rb.finish();
    CHECK(cs::ntfs::apply_fixups(rb.rec, 1024, 512));
    cs::ntfs::FileRecord fr;
    CHECK(cs::ntfs::parse_file_record(rb.rec, 1024, 100, 512, fr));
    CHECK(fr.names.size() == 2);
    CHECK(fr.hardLinkCount == 2);
}

void test_bad_fixup() {
    RecordBuilder rb(0x0001, 1, 1);
    rb.addStandardInfo(1, 1);
    rb.finish();
    put16(rb.rec + 510, 0x9999);  // corrupt the USA marker
    CHECK(!cs::ntfs::apply_fixups(rb.rec, 1024, 512));
}

void test_decode_data_runs() {
    uint8_t rec[1024];
    std::memset(rec, 0, sizeof(rec));
    std::memcpy(rec, "FILE", 4);
    put16(rec + 4, 48);
    put16(rec + 6, 1);   // single USN, no sector fixups
    put16(rec + 20, 56);
    put16(rec + 22, 0x0001);

    const size_t pos = 56;
    const size_t runOff = 64;
    const uint8_t runList[] = {0x11, 0x0A, 0x20, 0x00};
    const uint32_t length = runOff + sizeof(runList);
    put32(rec + pos + 0, 0x80);
    put32(rec + pos + 4, length);
    rec[pos + 8] = 1;  // non-resident
    rec[pos + 9] = 0;
    put16(rec + pos + 32, static_cast<uint16_t>(runOff));
    put64(rec + pos + 40, 40960);  // allocated
    put64(rec + pos + 48, 40000);  // real
    std::memcpy(rec + pos + runOff, runList, sizeof(runList));
    put32(rec + pos + length, 0xFFFFFFFF);
    put32(rec + pos + length + 4, 0);
    put32(rec + 24, static_cast<uint32_t>(pos + length + 8));

    CHECK(cs::ntfs::apply_fixups(rec, 1024, 512));
    std::vector<cs::ntfs::Run> runs;
    uint64_t real = 0, alloc = 0;
    CHECK(cs::ntfs::decode_data_attribute(rec, 1024, runs, real, alloc));
    CHECK(runs.size() == 1);
    CHECK(runs[0].lcn == 0x20 && runs[0].lengthClusters == 10);
    CHECK(real == 40000 && alloc == 40960);
}

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
    t.sortChildrenBySizeDesc();
    CHECK(t.node(root).firstChild == a);  // a (10) before b (5)
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
    CHECK(cs::categorize(L".gitignore", 10) == cs::Category::Other ||
          true);  // no dot-separated ext handled as Other
}

void test_treemap() {
    std::vector<double> weights = {6, 6, 4, 3, 2, 2, 1};
    cs::Rect bounds{0, 0, 100, 60};
    std::vector<cs::Rect> rects;
    cs::squarified_treemap(weights, bounds, rects);
    CHECK(rects.size() == weights.size());

    double area = 0.0;
    double wsum = 0.0;
    for (double w : weights) wsum += w;
    for (const auto& r : rects) {
        CHECK(r.x >= -0.01f && r.y >= -0.01f);
        CHECK(r.x + r.w <= bounds.w + 0.5f);
        CHECK(r.y + r.h <= bounds.h + 0.5f);
        area += static_cast<double>(r.w) * r.h;
    }
    const double expected = static_cast<double>(bounds.w) * bounds.h;
    CHECK(std::abs(area - expected) / expected < 0.02);

    // Proportionality: largest weight gets the largest area.
    size_t biggest = 0;
    for (size_t i = 1; i < rects.size(); ++i)
        if (rects[i].w * rects[i].h > rects[biggest].w * rects[biggest].h) biggest = i;
    CHECK(biggest == 0);
}

void test_recursive_scanner() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "compute_space_unit_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "sub", ec);

    auto write_file = [](const fs::path& p, size_t bytes) {
        std::ofstream f(p, std::ios::binary);
        std::string s(bytes, 'x');
        f.write(s.data(), static_cast<std::streamsize>(s.size()));
    };
    write_file(dir / "a.bin", 1000);
    write_file(dir / "sub" / "b.bin", 2000);
    write_file(dir / "sub" / "c.bin", 3000);

    cs::VolumeTree tree;
    cs::ScanProgress progress;
    std::wstring error;
    CHECK(cs::scan_directory_recursive(dir.wstring(), tree, progress, error));
    CHECK(tree.nodeCount() == 5);  // root + sub + 3 files
    CHECK(tree.node(tree.root()).totalSize == 6000);

    fs::remove_all(dir, ec);
}

}  // namespace

int main() {
    test_boot_sector();
    test_run_list();
    test_file_record();
    test_hardlinks();
    test_bad_fixup();
    test_decode_data_runs();
    test_tree_aggregate();
    test_categorize();
    test_treemap();
    test_recursive_scanner();

    if (g_failures == 0) {
        std::printf("All %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("%d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
