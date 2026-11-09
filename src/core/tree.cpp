#include "tree.hpp"

#include <algorithm>
#include <cwchar>

// 本文件实现 VolumeTree：一个 arena（平坦数组 + uint32 下标）形式的目录树。
// 之所以不用“每个节点一个 new 出来的指针树”，是因为真实卷上节点数可达数百万：
// 指针树会产生等量的小块堆分配、把节点打散到内存各处（缓存局部性极差），并且
// 析构时要逐节点释放。arena 只有一个 Node 数组加一个字符池，扩容后依然连续，
// 遍历与统计都是顺序访问。代价是：节点下标（uint32_t）只在当前树、当前次扫描内
// 有效，clear() 之后全部作废。
//
// 文件名也不放进节点，而是集中存在字符池 names_ 里，节点只保存
// (nameOffset, nameLength)。每个节点因此省掉一次 std::wstring 的堆分配，短名字
// 也不会造成大量碎片；代价是名字池在扫描期间只增不减，偏移才保持稳定。
// nameLength 用 uint16_t 存：NTFS 单个文件名的上限是 255 个字符，足够。

namespace cs {
namespace {

// 扩展名比较：ext 是文件名里 '.' 之后的宽字符后缀（长度已知，不以 0 结尾），
// ascii 是下方分类表里的纯 ASCII 小写串。
// 只手工折叠 ASCII 的 A-Z，不调用 towlower，避免行为随 locale 变化；末尾还要求
// ascii 已走到 0，因此短后缀不会误匹配长条目（"c" 不会命中 "cpp"）。
bool iequals_ext(const wchar_t* ext, size_t len, const char* ascii) {
    for (size_t i = 0; i < len; ++i) {
        char c = ascii[i];
        if (c == 0) return false;
        wchar_t w = ext[i];
        if (w >= L'A' && w <= L'Z') w = static_cast<wchar_t>(w - L'A' + L'a');
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (w != static_cast<wchar_t>(c)) return false;
    }
    return ascii[len] == 0;
}

// 分类表统一以 nullptr 结尾，省掉额外的长度参数；命中即返回。
bool in_list(const wchar_t* ext, size_t len, const char* const* list) {
    for (const char* const* p = list; *p; ++p) {
        if (iequals_ext(ext, len, *p)) return true;
    }
    return false;
}

}  // namespace

// 按扩展名归类。length 是 wchar_t 个数（不是字节数），name 不要求以 0 结尾。
// 只认最后一个 '.'：隐藏文件 ".gitignore" 的 '.' 落在下标 0，后缀为空，归为
// Other；"archive.tar.gz" 只看 "gz"（只看末级后缀，不做复合后缀推断）。
// 纯 ASCII 表意味着非 ASCII 扩展名一律落到 Other，这是刻意的取舍：
// 分类只影响 UI 着色/分组，不影响大小统计。
Category categorize(const wchar_t* name, uint32_t length) {
    if (length == 0) return Category::Other;
    uint32_t dot = length;
    for (uint32_t i = length; i > 0; --i) {
        if (name[i - 1] == L'.') { dot = i - 1; break; }
    }
    if (dot == length) return Category::Other;  // 没有 '.'，无扩展名
    const wchar_t* ext = name + dot + 1;
    const size_t len = length - dot - 1;
    if (len == 0) return Category::Other;

    // 静态表只读，放在函数内可避免额外符号导出；按常见度排列并无语义。
    static const char* const kVideo[] = {"mp4","mkv","avi","mov","wmv","flv","webm","m4v","mpg","mpeg","ts","m2ts",nullptr};
    static const char* const kImage[] = {"jpg","jpeg","png","gif","bmp","tif","tiff","webp","heic","svg","ico","raw","cr2","nef",nullptr};
    static const char* const kAudio[] = {"mp3","wav","flac","aac","ogg","wma","m4a","opus","aiff",nullptr};
    static const char* const kDoc[]   = {"pdf","doc","docx","xls","xlsx","ppt","pptx","txt","md","rtf","odt","ods","csv","epub",nullptr};
    static const char* const kArchive[] = {"zip","rar","7z","tar","gz","bz2","xz","iso","cab","tgz",nullptr};
    static const char* const kExec[]  = {"exe","dll","msi","sys","com","bin","so","dylib","scr","cpl",nullptr};
    static const char* const kCode[]  = {"c","cpp","cc","cxx","h","hpp","cs","java","py","js","ts","tsx","jsx","html","css","json","xml","go","rs","sh","bat","ps1","yml","yaml",nullptr};

    if (in_list(ext, len, kVideo))   return Category::Video;
    if (in_list(ext, len, kImage))   return Category::Image;
    if (in_list(ext, len, kAudio))   return Category::Audio;
    if (in_list(ext, len, kDoc))     return Category::Document;
    if (in_list(ext, len, kArchive)) return Category::Archive;
    if (in_list(ext, len, kExec))    return Category::Executable;
    if (in_list(ext, len, kCode))    return Category::Code;
    return Category::Other;
}

// clear() 只清空容器并把根标记为“无”，不释放 vector 的容量（clear 不改变
// capacity）。这样同一进程里换卷/重扫时可以复用已分配的内存，避免再次付出
// 数百万节点的分配成本；但 clear() 之后所有旧下标都失效。
void VolumeTree::clear() {
    nodes_.clear();
    names_.clear();
    root_ = kInvalid;
}

// 预分配用于在扫描前一次性预留，避免 vector 反复扩容搬移（数百万节点时搬移
// 会成倍放大内存峰值）。nameChars 是名字池的 wchar_t 个数，可按
// “记录数 × 16”这类经验值估算，宁大勿小。
void VolumeTree::reserve(size_t nodeCount, size_t nameChars) {
    nodes_.reserve(nodeCount);
    names_.reserve(nameChars);
}

// 追加一个尚未挂入层级的新节点，返回它在 nodes_ 中的下标。
// 传入的 size/alloc 是“节点自身”的大小（字节，uint64_t），先写进
// totalSize/totalAlloc；aggregate() 之后这两个字段会被改写为子树累计值。
// 同名文件可能有多个 $FILE_NAME（硬链接），调用方应为每个目录项各建一个节点，
// recordIndex 记录来源 MFT 记录号，用于诊断/去重，运行时不参与链接。
uint32_t VolumeTree::addNode(const wchar_t* name, uint32_t nameLength, uint64_t size,
                             uint64_t alloc, bool isDirectory, uint32_t recordIndex) {
    Node n;
    n.parent = kInvalid;
    n.firstChild = kInvalid;
    n.nextSibling = kInvalid;
    // 名字不单独分配：追加到全局字符池 names_，节点只存偏移 + 长度。
    // 这样避免了每个节点一个 std::wstring 的堆开销（每个至少两次分配），
    // 但也意味着名字池在扫描期间不得收缩，偏移才保持稳定。
    // nameOffset 是 uint32_t：名字池最大按 4G 个 wchar_t 定位，对任何现实卷都够用；
    // nameLength 是 uint16_t，超长名字会被截断而不是越界读，调用方需保证不超 65535。
    n.nameOffset = static_cast<uint32_t>(names_.size());
    n.nameLength = static_cast<uint16_t>(nameLength);
    n.flags = isDirectory ? 1 : 0;
    // 目录不按扩展名分类（目录名里的 '.' 不代表类型），统一为 Other。
    n.category = static_cast<uint8_t>(isDirectory ? Category::Other
                                                  : categorize(name, nameLength));
    n.totalSize = size;
    n.totalAlloc = alloc;
    n.childCount = 0;
    n.recordIndex = recordIndex;
    names_.insert(names_.end(), name, name + nameLength);
    names_.push_back(L'\0');  // 补 0 终结符，name() 可直接当 C 字符串打印，无需拷贝

    const uint32_t index = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back(n);
    return index;
}

// 把 child 挂到 parent 的兄弟链头部（O(1) 头插），因此 link 之后同一层的顺序
// 是“后添加的在前”，是任意的；最终顺序由 sortChildrenBySizeDesc() 决定。
// 只负责建立父子关系和 childCount，不校验 child 是否已经被链接过：
// 同一节点被 link 两次会形成环或让兄弟链重复出现，调用方必须保证每个节点只
// link 一次。下标非法（含越界、自环）时静默忽略，便于扫描器做宽容处理。
void VolumeTree::link(uint32_t parent, uint32_t child) {
    if (!valid(parent) || !valid(child) || parent == child) return;
    Node& c = nodes_[child];
    // parent 记在子节点上（而不是父节点的子列表里）：链接完之后，
    // 扫描器靠 parent == kInvalid 就能一眼找出没能挂上树的孤儿节点。
    c.parent = parent;
    Node& p = nodes_[parent];
    c.nextSibling = p.firstChild;
    p.firstChild = child;
    ++p.childCount;
}

// 名字池里已经带了终结符，这里仍显式构造 std::wstring 并指定长度，
// 是为了不依赖“名字中间不会出现 0”这一假设（POSIX 命名空间下可能成立）。
// name() 走的是零拷贝路径（直接返回池内指针，下次 addNode 前有效）；
// 需要跨调用长期持有名字时用本函数换成一个独立副本。
std::wstring VolumeTree::nameString(uint32_t i) const {
    if (!valid(i)) return {};
    const Node& n = nodes_[i];
    return std::wstring(names_.data() + n.nameOffset, n.nameLength);
}

// 自底向上把子树大小累加到每个节点的 totalSize/totalAlloc（字节，uint64_t）。
//
// 约束：必须在整棵树链接完毕之后、且只调用一次。它把 totalSize 从“节点自身
// 大小”原地改写为“子树累计大小”，重复调用会把子节点已经被累加过的值再加一遍，
// 结果偏大且没有报错。
//
// 这里用显式栈做一次前序遍历，再反向遍历（等价于后序）：这样不需要递归，
// 深度上万的目录也不会爆栈；同时避免了按“子节点全部就绪”做二次扫描。
void VolumeTree::aggregate() {
    if (!valid(root_)) return;
    std::vector<uint32_t> order;
    order.reserve(nodes_.size());
    std::vector<uint32_t> stack;
    stack.push_back(root_);
    while (!stack.empty()) {
        const uint32_t u = stack.back();
        stack.pop_back();
        order.push_back(u);
        for (uint32_t c = nodes_[u].firstChild; c != kInvalid; c = nodes_[c].nextSibling)
            stack.push_back(c);
    }
    // 逆前序 == 每个子节点都先于其父节点被处理，于是累加时子节点的值已是子树和。
    // 未被链接到根的子树上不可达，自然不参与统计（孤儿的数量由扫描器单独报告）。
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const uint32_t u = *it;
        Node& n = nodes_[u];
        uint64_t size = n.totalSize;   // 此刻还是节点自身大小
        uint64_t alloc = n.totalAlloc;
        for (uint32_t c = n.firstChild; c != kInvalid; c = nodes_[c].nextSibling) {
            size += nodes_[c].totalSize;
            alloc += nodes_[c].totalAlloc;
        }
        n.totalSize = size;
        n.totalAlloc = alloc;
    }
    // 累加全程用 uint64_t 字节计数，不做饱和处理：要溢出需要 2^64 字节的子树，
    // 远超任何现实卷；真溢出也只影响显示，不会造成越界。
}

// 按子树逻辑大小降序重排“每一层”的兄弟链（不是对整个数组重排，节点下标不变）。
//
// 前提：必须紧跟在 aggregate() 之后调用。否则比较的是各节点自身的大小，
// 目录会被当成 0 排到最底下，结果看起来像“空目录优先”。
// 副作用：改写 firstChild 与每个子节点的 nextSibling，遍历顺序随之变化；
// 若 UI 正在遍历同一棵树，需要外部保证不并发调用。
// 大小相同时按节点下标升序，保证结果稳定可复现（不依赖 std::sort 的内部顺序）。
// 复杂度是各层 O(k log k) 之和；即使百万级节点，整体也只在排序上花几十毫秒。
void VolumeTree::sortChildrenBySizeDesc() {
    std::vector<uint32_t> children;
    for (uint32_t i = 0; i < nodes_.size(); ++i) {
        Node& n = nodes_[i];
        if (n.childCount < 2) continue;  // 0/1 个子节点无需排序，省掉一次分配
        children.clear();
        for (uint32_t c = n.firstChild; c != kInvalid; c = nodes_[c].nextSibling)
            children.push_back(c);
        std::sort(children.begin(), children.end(), [this](uint32_t a, uint32_t b) {
            if (nodes_[a].totalSize != nodes_[b].totalSize)
                return nodes_[a].totalSize > nodes_[b].totalSize;
            return a < b;
        });
        // 只重建链指针，不移动 Node：父子关系、名字池偏移、recordIndex 全部不动，
        // 因此外部持有的节点下标在排序后依然有效。
        n.firstChild = children.empty() ? kInvalid : children.front();
        for (size_t k = 0; k < children.size(); ++k) {
            nodes_[children[k]].nextSibling = (k + 1 < children.size()) ? children[k + 1] : kInvalid;
        }
    }
}

}  // namespace cs
