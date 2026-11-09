// 开发/QA 工具：无界面的 MFT 扫描，打印诊断统计，并可选地与递归扫描器做
// 逐目录交叉比对。打开 \\.\X: 需要管理员权限，普通权限下会直接报错退出。
//
//   mft_scan <volume e.g. C:> [dir-to-cross-check] [out-report.txt]
//
// 三个参数都可省略：缺省扫描 C:、不做交叉比对、不写报告文件。
// 报告是纯 ASCII 统计加 UTF-8 路径，适合在 CI 里存档后做前后对比。

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

// 只引入两个最常用的类型名，避免在全文里反复写 cs:: 前缀；
// 注意 human_size/wide_to_utf8 仍然带限定名调用，以保持「这是工具函数」的醒目度。
using cs::ScanProgress;
using cs::VolumeTree;

// 匿名命名空间：g_report、addf 等仅供本文件使用，放进内部链接可以避免与
// 其他诊断工具（如 mft_record.cpp）里的同名符号在链接期冲突。
namespace {

// 整份报告先攒在内存里，最后一次性输出。
// 之所以不边算边打印：报告要同时落到 stdout 和文件，而差异比较的输出与统计
// 是交错产生的，攒完再写能保证两处字节完全一致（文件按二进制模式写，
// 不做 CRLF 转换，避免把 LF 变成 CRLF 后破坏与基线文件的比对）。
std::string g_report;

// printf 风格的追加函数。
// 报告行都很短，2048 字节栈缓冲足够；用固定缓冲而非 std::string 拼接，
// 是为了保留 printf 的格式化便利，同时不在工具里引入额外的分配路径。
// 超长内容会被 vsnprintf 截断——对诊断工具可以接受，不会因此崩溃。
void addf(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_report += buf;
}

// 在兄弟链表中按名称线性查找直接子节点。
// 不能二分：子节点是按子树大小降序排列的，不是按名称排序。
// 未找到返回 kInvalid，调用方必须显式判断，否则随后调用 node() 会越界。
uint32_t find_child(const VolumeTree& t, uint32_t parent, const std::wstring& name) {
    for (uint32_t c = t.node(parent).firstChild; c != VolumeTree::kInvalid;
         c = t.node(c).nextSibling) {
        if (t.nameString(c) == name) return c;
    }
    return VolumeTree::kInvalid;
}

// 把 "C:\a\b" 或 "\a\b" 拆成 {"a","b"}，用于沿着 MFT 树的层级下钻到
// 递归扫描的根目录所在的位置。
// - 先去掉 "X:" 形式的盘符（只认第 2 个字符是冒号的情况，避免误伤其他路径）；
// - 再去掉所有前导分隔符；
// - 连续分隔符或结尾分隔符会产生空分量，这里直接跳过而不是报错；
// - 同时接受 '\' 和 '/'，因为路径从命令行复制进来时两种都可能出现。
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

// 差异比较的上下文。两个树指针都不拥有对象，生命周期由 wmain 保证。
// printed/max_print 是全局输出上限：真实卷的差异可能上万条，全部打印既慢
// 也没人看，超出后静默丢弃（统计行仍会提示范围）。
// max_depth 限制递归深度，避免在极深的目录树上做无意义的逐层展开。
struct DiffState {
    const VolumeTree* mft = nullptr;
    const VolumeTree* rec = nullptr;
    int printed = 0;
    int max_print = 300;
    int max_depth = 5;
};

// 递归比较两棵树的同名子树，把差异追加到报告。
//
// 比较的是 aggregate() 之后的 totalSize（整棵子树的总和），所以父节点一致
// 就无需下钻；父节点不一致时继续往下定位到具体目录。
// 注意 `if (st.printed++ < st.max_print)` 无论是否输出都会自增，
// 这是刻意的：目的是限制「被检查到的差异条数」，而不是精确的打印行数。
// 另外只有「两边都是目录、MFT 侧还有子节点、且未到深度上限」才递归，
// 否则直接打一条 DIFF，避免差异粒度碎到无法阅读。
void diff_tree(DiffState& st, uint32_t mnode, uint32_t rnode, const std::string& path,
               int depth) {
    const VolumeTree& m = *st.mft;
    const VolumeTree& r = *st.rec;
    // 第一轮：以 MFT 树为基准，检查每个子项在递归扫描结果中是否存在。
    for (uint32_t c = m.node(mnode).firstChild; c != VolumeTree::kInvalid;
         c = m.node(c).nextSibling) {
        const std::wstring name = m.nameString(c);
        const uint64_t ms = m.node(c).totalSize;
        const uint32_t rm = find_child(r, rnode, name);
        const std::string childPath = path + "\\" + cs::wide_to_utf8(name);
        if (rm == VolumeTree::kInvalid) {
            // 递归扫描里没有这个名称：可能是权限导致该目录被跳过，
            // 也可能是硬链接的另一个命名项（两边取名策略不同）。
            if (st.printed++ < st.max_print)
                addf("  ONLY-IN-MFT %s = %s\n", childPath.c_str(),
                     cs::human_size(ms).c_str());
            continue;
        }
        const uint64_t rs = r.node(rm).totalSize;
        // 子树总量相同即视为一致，不再下钻——这是本工具的判定口径，
        // 因此两边「总量相同但分布不同」的情况不会被发现。
        if (ms == rs) continue;
        const bool mdir = m.isDirectory(c);
        const bool rdir = r.isDirectory(rm);
        // 仅当两侧都是目录、MFT 侧还有子节点时才继续细化差异。
        if (depth < st.max_depth && mdir && rdir && m.node(c).childCount > 0) {
            diff_tree(st, c, rm, childPath, depth + 1);
        } else if (st.printed++ < st.max_print) {
            addf("  DIFF %s  mft=%s rec=%s\n", childPath.c_str(),
                 cs::human_size(ms).c_str(), cs::human_size(rs).c_str());
        }
    }
    // 第二轮：反过来检查只在递归扫描里出现的项，覆盖上面第一轮看不到的方向。
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

// 用 wmain 而不是 main：参数是 wchar_t**，卷名和路径可以包含非 ANSI 字符，
// 无需再做命令行编码转换。返回值即进程退出码：0 成功、1 扫描失败。
int wmain(int argc, wchar_t** argv) {
    // 参数全部可选。卷名用不带反斜杠的 "C:" 形式，与 scan_volume_mft 的约定一致。
    const std::wstring volume = (argc > 1) ? argv[1] : L"C:";
    const std::wstring check_dir = (argc > 2) ? argv[2] : L"";
    const std::wstring out_path = (argc > 3) ? argv[3] : L"";

    // 全新构造的 ScanProgress 各原子量都是 0，因此这里不需要调用 reset()；
    // 反过来说，如果调用 reset() 也会把可能已设置的 cancel 清掉。
    VolumeTree mft;
    ScanProgress progress;
    std::wstring error;
    const bool ok = cs::scan_volume_mft(volume, mft, progress, error);

    if (!ok) {
        // 失败时 tree 已被清空，只报告错误原因；退出码随后返回 1，便于脚本判断。
        addf("MFT scan FAILED: %s\n", cs::wide_to_utf8(error).c_str());
    } else {
        addf("=== MFT scan of %s ===\n", cs::wide_to_utf8(volume).c_str());
        // 以下统计全部读取原子计数器：这些值彼此不保证同一时刻，
        // 只用于观察量级，不能当作严格一致的账目。
        addf("records    = %llu\n",
             static_cast<unsigned long long>(progress.totalRecords.load()));
        addf("files      = %llu\n",
             static_cast<unsigned long long>(progress.fileCount.load()));
        addf("dirs       = %llu\n",
             static_cast<unsigned long long>(progress.dirCount.load()));
        // 孤立节点的总量：这些字节通常不出现在任何目录路径里，但对不上账时
        // 它们就是差额来源，因此单独列出。
        addf("orphans    = %llu nodes, %llu bytes (%s)\n",
             static_cast<unsigned long long>(progress.orphanNodes.load()),
             static_cast<unsigned long long>(progress.orphanBytes.load()),
             cs::human_size(progress.orphanBytes.load()).c_str());
        addf("  noParent    = %llu\n",
             static_cast<unsigned long long>(progress.orphanNoParent.load()));
        addf("  seqMismatch = %llu\n",
             static_cast<unsigned long long>(progress.orphanSeqMismatch.load()));
        // noParent 的四个细分原因：outOfRange/notInUse/notDir 基本说明父记录
        // 已被回收，noNode 则可能是父目录本身没有可用的 $FILE_NAME。
        addf("  parent-missing reason: outOfRange=%llu notInUse=%llu notDir=%llu noNode=%llu\n",
             static_cast<unsigned long long>(progress.orphanParentOutOfRange.load()),
             static_cast<unsigned long long>(progress.orphanParentNotInUse.load()),
             static_cast<unsigned long long>(progress.orphanParentNotDir.load()),
             static_cast<unsigned long long>(progress.orphanParentNoNode.load()));
        // 记录级拒绝统计：正常卷上 notInUse 通常很大（历史删除的记录），
        // fixupFail/parseFail 才是值得警惕的信号。
        addf("records: notInUse=%llu metadata=%llu noName=%llu fixupFail=%llu parseFail=%llu\n",
             static_cast<unsigned long long>(progress.recordsNotInUse.load()),
             static_cast<unsigned long long>(progress.recordsMetadata.load()),
             static_cast<unsigned long long>(progress.recordsNoName.load()),
             static_cast<unsigned long long>(progress.recordsFixupFail.load()),
             static_cast<unsigned long long>(progress.recordsParseFail.load()));
        // size-follow 反映大文件/碎片文件走 $ATTRIBUTE_LIST 回填大小的比例；
        // attempts 远大于 success 时说明扩展记录读取经常失败。
        addf("size-follow: attempts=%llu success=%llu\n",
             static_cast<unsigned long long>(progress.sizeFollowAttempts.load()),
             static_cast<unsigned long long>(progress.sizeFollowSuccess.load()));
        // 根节点的 totalSize 已由 aggregate() 汇总，是整卷可见数据量的参考值。
        addf("root total = %llu (%s)\n",
             static_cast<unsigned long long>(mft.node(mft.root()).totalSize),
             cs::human_size(mft.node(mft.root()).totalSize).c_str());

        // 顶层子项按大小降序（sortChildrenBySizeDesc 已在扫描内完成），
        // 名称左对齐 30 列、大小右对齐 14 列，方便肉眼扫读。
        // 注意 %-30s 的宽度按字节计，中文名的 UTF-8 字节数多于显示宽度，
        // 所以含中文的项在终端里会略微错位——这是显示问题，不影响数值。
        addf("\n--- top-level children ---\n");
        for (uint32_t c = mft.node(mft.root()).firstChild; c != VolumeTree::kInvalid;
             c = mft.node(c).nextSibling) {
            addf("  %-30s %14s\n", cs::wide_to_utf8(mft.nameString(c)).c_str(),
                 cs::human_size(mft.node(c).totalSize).c_str());
        }
    }

    // 交叉比对只在 MFT 扫描成功且给了目录时才有意义。
    // 比对口径是两边的 totalSize（逻辑大小总和），不比较 totalAlloc：
    // 稀疏/压缩文件在「逻辑」和「占盘」两种口径下的差值本来就不同，
    // 混用会制造大量无意义的差异行。
    if (ok && !check_dir.empty()) {
        // 用独立的树和进度对象：绝不能复用 mft/progress，
        // 否则会把已建好的结果覆盖掉，比较也就无从谈起。
        VolumeTree rec;
        ScanProgress rp;
        std::wstring rerr;
        addf("\n=== cross-check: %s ===\n", cs::wide_to_utf8(check_dir).c_str());
        if (!cs::scan_directory_recursive(check_dir, rec, rp, rerr)) {
            // 递归扫描失败通常是无权限或路径不存在；这一步失败不影响前面的结果。
            addf("recursive scan failed\n");
        } else {
            addf("recursive total = %s\n",
                 cs::human_size(rec.node(rec.root()).totalSize).c_str());

            // 递归扫描的根就是 check_dir 本身，而 MFT 树的根是整卷，
            // 所以必须按路径分量逐级下钻，才能拿同一层级的子树做比较。
            uint32_t node = mft.root();
            for (const auto& comp : split_relative(check_dir)) {
                node = find_child(mft, node, comp);
                if (node == VolumeTree::kInvalid) break;
            }
            if (node == VolumeTree::kInvalid) {
                // 该路径在 MFT 树里找不到（常见于路径只在递归视角可见，
                // 或大小写/短名差异），此时放弃比较而不是直接判失败。
                addf("MFT: path not found in tree\n");
            } else {
                const uint64_t mft_total = mft.node(node).totalSize;
                const uint64_t rec_total = rec.node(rec.root()).totalSize;
                addf("MFT subtree total = %s\n", cs::human_size(mft_total).c_str());
                // 用有符号差值：正值表示 MFT 侧统计更大（常见原因：硬链接
                // 被多个目录项重复计数、稀疏文件按逻辑大小计）。负值则相反。
                addf("difference        = %lld bytes\n",
                     static_cast<long long>(mft_total) - static_cast<long long>(rec_total));

                addf("\n--- recursive diff (mft vs recursive) ---\n");
                // max_depth 从 check_dir 这一层开始计数（depth=0），
                // 因此 5 表示最多往下看 5 层；更深的差异会退化成一条 DIFF 汇总。
                DiffState st;
                st.mft = &mft;
                st.rec = &rec;
                diff_tree(st, node, rec.root(), cs::wide_to_utf8(check_dir), 0);
                // 一条差异都没有时显式说明，避免空洞的输出让人误以为程序没跑。
                if (st.printed == 0) addf("  (identical within depth)\n");
            }
        }
    }

    // 一次性输出报告。stdout 的编码是 UTF-8 字节流，控制台代码页不是 65001 时
    // 中文路径可能显示为乱码，但写入文件的内容始终是正确的。
    std::fputs(g_report.c_str(), stdout);
    if (!out_path.empty()) {
        // 用 _wfopen_s 打开宽字符路径（可处理中文路径），"wb" 保证不做换行转换；
        // 打开失败（目录不存在、无权限）会被静默忽略——stdout 上已经有完整报告。
        FILE* f = nullptr;
        _wfopen_s(&f, out_path.c_str(), L"wb");
        if (f != nullptr) {
            std::fwrite(g_report.data(), 1, g_report.size(), f);
            std::fclose(f);
        }
    }
    // 退出码反映 MFT 扫描是否成功，供批处理/CI 判断。
    return ok ? 0 : 1;
}
