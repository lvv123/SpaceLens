#pragma once

// 卷目录树的 arena（扁平数组）实现：一次建好，之后由 UI 逐层查询。
//
// 为什么不用「每个节点一个指针/一个对象」的常规树：真实卷上节点可达数百万，
// 逐节点 new 既慢又产生大量堆碎片。这里所有节点放在一个 std::vector 里，
// 用 32 位下标互相引用，节点地址的唯一来源是下标，因此：
// - 不能用节点指针做长期缓存（vector 扩容会让指针失效，但下标永远有效）；
// - 名称单独放在字符池里，Node 里只存 offset，避免每个节点各带一个字符串。
//
// 顺序约定：aggregate() 和 sortChildrenBySizeDesc() 之后，每个节点的子节点
// 是按子树大小降序排列的单向链表，UI 直接顺序遍历即可。

#include <cstdint>
#include <string>
#include <vector>

namespace cs {

// 文件类型分类。数值会被直接存进 Node::category（uint8_t），UI 图例还会
// 从 1 到 7 依次 static_cast 回来渲染，因此数值必须保持连续，
// 新类别只能追加在末尾，不要插到中间或调整现有顺序。
enum class Category : uint8_t {
    Other = 0,
    Video,
    Image,
    Audio,
    Document,
    Archive,
    Executable,
    Code,
};

// 按扩展名（大小写不敏感）分类。`length` 是名称的 wchar_t 个数，不是字节数。
// 只取最后一个 '.' 之后的部分；无扩展名、隐藏文件（如 ".gitignore"）以及
// 未知扩展名一律返回 Other。
Category categorize(const wchar_t* name, uint32_t length);

class VolumeTree {
public:
    // 无效下标哨兵。所有「指向子节点/兄弟/根」的字段都用它表示空，
    // 也用于 root_ 尚未设置的状态。0xFFFFFFFF 与 32 位节点上限一致。
    static constexpr uint32_t kInvalid = 0xFFFFFFFFu;

    // 一个树节点。除 totalSize/totalAlloc 外，其余字段在节点创建后基本不变。
    struct Node {
        uint32_t parent      = kInvalid;   // 父节点下标；未链接时为 kInvalid
        uint32_t firstChild  = kInvalid;   // 第一个子节点（排序后即最大的那个）
        uint32_t nextSibling = kInvalid;   // 同一父节点下的下一个兄弟
        uint32_t nameOffset  = 0;          // 在名称池中的起始下标（wchar_t 单位）
        uint16_t nameLength  = 0;          // 名称长度（wchar_t 单位，不含结尾 NUL）
        uint8_t  flags       = 0;   // bit0 = directory
        uint8_t  category    = 0;   // cs::Category
        // 子树总大小（逻辑字节数）。aggregate() 之前这里只是节点自身大小，
        // 之后变成「自身 + 全部后代」；两种语义共用同一字段是刻意的省内存设计。
        uint64_t totalSize   = 0;
        uint64_t totalAlloc  = 0;   // 子树磁盘占用（aggregate() 之后）
        uint32_t childCount  = 0;   // 直接子节点数量，UI 用它在展开前判断是否为空
        uint32_t recordIndex = 0;   // 来源 MFT 记录号；硬链接产生的多个节点会共享
    };

    // 清空全部节点、名称池，并把 root_ 复位为 kInvalid。
    // 会保留两个 vector 的容量，便于同一对象上连续发起多次扫描。
    void clear();
    // 预分配节点数与名称字符数。真实卷扫描前用 $MFT 记录数一次预留到位，
    // 避免扫描过程中反复扩容（扩容还会让已取得的元素指针失效）。
    void reserve(size_t nodeCount, size_t nameChars);

    // 追加一个节点但暂不挂入层级，返回其下标。
    // 名称会被复制进名称池并补一个 NUL，因此调用方之后可以立即释放原缓冲。
    // 注意 nameLength 是 uint16_t，超长名称会被截断——调用方需自行保证不超限。
    uint32_t addNode(const wchar_t* name, uint32_t nameLength, uint64_t size,
                     uint64_t alloc, bool isDirectory, uint32_t recordIndex);

    // 把 `child` 挂到 `parent` 下（在 aggregate() 排序之前顺序是任意的：
    // 这里用头插法，所以实际顺序与插入顺序相反）。
    // 会静默忽略非法下标和自链接（parent == child），这既是防呆，
    // 也避免损坏的 MFT 数据把树变成环。
    void link(uint32_t parent, uint32_t child);

    void setRoot(uint32_t root) { root_ = root; }
    uint32_t root() const { return root_; }

    size_t      nodeCount() const { return nodes_.size(); }
    // 注意：这里不做越界检查。调用前必须确认 valid(i)，否则是 UB。
    const Node& node(uint32_t i) const { return nodes_[i]; }
    // 非 const 版本供聚合/排序等就地修改使用；同样不做越界检查。
    Node&       node(uint32_t i) { return nodes_[i]; }
    bool        valid(uint32_t i) const { return i < nodes_.size(); }

    // 直接返回名称池中的指针（以 NUL 结尾，可直接当 C 字符串用于 ImGui）。
    // 前置条件：i 必须有效，否则会读到池外。
    const wchar_t* name(uint32_t i) const { return names_.data() + nodes_[i].nameOffset; }
    // 带边界检查的版本：下标无效时返回空串，适合处理来路不明的下标。
    std::wstring   nameString(uint32_t i) const;

    bool isDirectory(uint32_t i) const { return (nodes_[i].flags & 1) != 0; }

    // 从 root_ 自底向上汇总子树大小。必须在全部 link() 完成之后调用。
    // 实现上是「前序收集 + 逆序回加」，所以不需要递归，深目录也不会爆栈。
    // 重要：本函数不可重复调用——它把子节点大小加到已经含子树的父节点上，
    // 第二次执行会把子树重复计算一遍。
    void aggregate();

    // 把每个节点的子链表按 totalSize 降序重排。
    // 依赖 aggregate() 的结果，因此必须在它之后调用；大小相同时按节点下标
    // 排序，保证同一份数据每次扫描得到完全一致的顺序（便于截图对比）。
    void sortChildrenBySizeDesc();

private:
    std::vector<Node>    nodes_;   // 全部节点，下标即节点 ID
    std::vector<wchar_t> names_;   // 名称字符池，每个名称以 NUL 结尾
    uint32_t             root_ = kInvalid;   // 根节点下标，setRoot 后有效
};

}  // namespace cs
