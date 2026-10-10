// Dev/QA tool: headless MFT scan with diagnostics, optionally cross-checked
// against the recursive scanner. Requires administrator (to open the volume).
//
//   mft_scan <volume e.g. C:> [dir-to-cross-check] [out-report.txt]

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "format.hpp"
#include "scan.hpp"
#include "tree.hpp"
#include "utf8.hpp"

using cs::ScanProgress;
using cs::VolumeTree;

namespace {

std::string g_report;

void addf(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_report += buf;
}

uint32_t find_child(const VolumeTree& t, uint32_t parent, const std::wstring& name) {
    for (uint32_t c = t.node(parent).firstChild; c != VolumeTree::kInvalid;
         c = t.node(c).nextSibling) {
        if (t.nameString(c) == name) return c;
    }
    return VolumeTree::kInvalid;
}

std::vector<std::wstring> split_relative(const std::wstring& dir) {
    std::wstring rel = dir;
    if (rel.size() >= 2 && rel[1] == L':') rel = rel.substr(2);
    while (!rel.empty() && (rel.front() == L'\\' || rel.front() == L'/')) rel.erase(rel.begin());
    std::vector<std::wstring> comps;
    size_t i = 0;
    while (i < rel.size()) {
        size_t j = rel.find_first_of(L"\\/", i);
        if (j == std::wstring::npos) j = rel.size();
        if (j > i) comps.push_back(rel.substr(i, j - i));
        i = j + 1;
    }
    return comps;
}

struct DiffState {
    const VolumeTree* mft = nullptr;
    const VolumeTree* rec = nullptr;
    int printed = 0;
    int max_print = 300;
    int max_depth = 5;
};

void diff_tree(DiffState& st, uint32_t mnode, uint32_t rnode, const std::string& path,
               int depth) {
    const VolumeTree& m = *st.mft;
    const VolumeTree& r = *st.rec;
    for (uint32_t c = m.node(mnode).firstChild; c != VolumeTree::kInvalid;
         c = m.node(c).nextSibling) {
        const std::wstring name = m.nameString(c);
        const uint64_t ms = m.node(c).totalSize;
        const uint32_t rm = find_child(r, rnode, name);
        const std::string childPath = path + "\\" + cs::wide_to_utf8(name);
        if (rm == VolumeTree::kInvalid) {
            if (st.printed++ < st.max_print)
                addf("  ONLY-IN-MFT %s = %s\n", childPath.c_str(),
                     cs::human_size(ms).c_str());
            continue;
        }
        const uint64_t rs = r.node(rm).totalSize;
        if (ms == rs) continue;
        const bool mdir = m.isDirectory(c);
        const bool rdir = r.isDirectory(rm);
        if (depth < st.max_depth && mdir && rdir && m.node(c).childCount > 0) {
            diff_tree(st, c, rm, childPath, depth + 1);
        } else if (st.printed++ < st.max_print) {
            addf("  DIFF %s  mft=%s rec=%s\n", childPath.c_str(),
                 cs::human_size(ms).c_str(), cs::human_size(rs).c_str());
        }
    }
    for (uint32_t c = r.node(rnode).firstChild; c != VolumeTree::kInvalid;
         c = r.node(c).nextSibling) {
        const std::wstring name = r.nameString(c);
        if (find_child(m, mnode, name) == VolumeTree::kInvalid && st.printed++ < st.max_print) {
            addf("  ONLY-IN-REC %s = %s\n",
                 (path + "\\" + cs::wide_to_utf8(name)).c_str(),
                 cs::human_size(r.node(c).totalSize).c_str());
        }
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring volume = (argc > 1) ? argv[1] : L"C:";
    const std::wstring check_dir = (argc > 2) ? argv[2] : L"";
    const std::wstring out_path = (argc > 3) ? argv[3] : L"";

    VolumeTree mft;
    ScanProgress progress;
    std::wstring error;
    const bool ok = cs::scan_volume_mft(volume, mft, progress, error);

    if (!ok) {
        addf("MFT scan FAILED: %s\n", cs::wide_to_utf8(error).c_str());
    } else {
        addf("=== MFT scan of %s ===\n", cs::wide_to_utf8(volume).c_str());
        addf("records    = %llu\n",
             static_cast<unsigned long long>(progress.totalRecords.load()));
        addf("files      = %llu\n",
             static_cast<unsigned long long>(progress.fileCount.load()));
        addf("dirs       = %llu\n",
             static_cast<unsigned long long>(progress.dirCount.load()));
        addf("orphans    = %llu nodes, %llu bytes (%s)\n",
             static_cast<unsigned long long>(progress.orphanNodes.load()),
             static_cast<unsigned long long>(progress.orphanBytes.load()),
             cs::human_size(progress.orphanBytes.load()).c_str());
        addf("  noParent    = %llu\n",
             static_cast<unsigned long long>(progress.orphanNoParent.load()));
        addf("  seqMismatch = %llu\n",
             static_cast<unsigned long long>(progress.orphanSeqMismatch.load()));
        addf("  parent-missing reason: outOfRange=%llu notInUse=%llu notDir=%llu noNode=%llu\n",
             static_cast<unsigned long long>(progress.orphanParentOutOfRange.load()),
             static_cast<unsigned long long>(progress.orphanParentNotInUse.load()),
             static_cast<unsigned long long>(progress.orphanParentNotDir.load()),
             static_cast<unsigned long long>(progress.orphanParentNoNode.load()));
        addf("records: notInUse=%llu metadata=%llu noName=%llu fixupFail=%llu parseFail=%llu\n",
             static_cast<unsigned long long>(progress.recordsNotInUse.load()),
             static_cast<unsigned long long>(progress.recordsMetadata.load()),
             static_cast<unsigned long long>(progress.recordsNoName.load()),
             static_cast<unsigned long long>(progress.recordsFixupFail.load()),
             static_cast<unsigned long long>(progress.recordsParseFail.load()));
        addf("size-follow: attempts=%llu success=%llu\n",
             static_cast<unsigned long long>(progress.sizeFollowAttempts.load()),
             static_cast<unsigned long long>(progress.sizeFollowSuccess.load()));
        addf("root total = %llu (%s)\n",
             static_cast<unsigned long long>(mft.node(mft.root()).totalSize),
             cs::human_size(mft.node(mft.root()).totalSize).c_str());

        addf("\n--- top-level children ---\n");
        for (uint32_t c = mft.node(mft.root()).firstChild; c != VolumeTree::kInvalid;
             c = mft.node(c).nextSibling) {
            addf("  %-30s %14s\n", cs::wide_to_utf8(mft.nameString(c)).c_str(),
                 cs::human_size(mft.node(c).totalSize).c_str());
        }
    }

    if (ok && !check_dir.empty()) {
        VolumeTree rec;
        ScanProgress rp;
        std::wstring rerr;
        addf("\n=== cross-check: %s ===\n", cs::wide_to_utf8(check_dir).c_str());
        if (!cs::scan_directory_recursive(check_dir, rec, rp, rerr)) {
            addf("recursive scan failed\n");
        } else {
            addf("recursive total = %s\n",
                 cs::human_size(rec.node(rec.root()).totalSize).c_str());

            uint32_t node = mft.root();
            for (const auto& comp : split_relative(check_dir)) {
                node = find_child(mft, node, comp);
                if (node == VolumeTree::kInvalid) break;
            }
            if (node == VolumeTree::kInvalid) {
                addf("MFT: path not found in tree\n");
            } else {
                const uint64_t mft_total = mft.node(node).totalSize;
                const uint64_t rec_total = rec.node(rec.root()).totalSize;
                addf("MFT subtree total = %s\n", cs::human_size(mft_total).c_str());
                addf("difference        = %lld bytes\n",
                     static_cast<long long>(mft_total) - static_cast<long long>(rec_total));

                addf("\n--- recursive diff (mft vs recursive) ---\n");
                DiffState st;
                st.mft = &mft;
                st.rec = &rec;
                diff_tree(st, node, rec.root(), cs::wide_to_utf8(check_dir), 0);
                if (st.printed == 0) addf("  (identical within depth)\n");
            }
        }
    }

    std::fputs(g_report.c_str(), stdout);
    if (!out_path.empty()) {
        FILE* f = nullptr;
        _wfopen_s(&f, out_path.c_str(), L"wb");
        if (f != nullptr) {
            std::fwrite(g_report.data(), 1, g_report.size(), f);
            std::fclose(f);
        }
    }
    return ok ? 0 : 1;
}
