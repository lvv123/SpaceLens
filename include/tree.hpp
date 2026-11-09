#pragma once

// Arena (flat-array) volume tree. Built once, then queried by the UI.
// Children are kept in a singly linked list ordered by descending subtree size
// after aggregate()/sortChildrenBySizeDesc().

#include <cstdint>
#include <string>
#include <vector>

namespace cs {

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

// File-type classification by extension (case-insensitive).
Category categorize(const wchar_t* name, uint32_t length);

class VolumeTree {
public:
    static constexpr uint32_t kInvalid = 0xFFFFFFFFu;

    struct Node {
        uint32_t parent      = kInvalid;
        uint32_t firstChild  = kInvalid;
        uint32_t nextSibling = kInvalid;
        uint32_t nameOffset  = 0;
        uint16_t nameLength  = 0;
        uint8_t  flags       = 0;   // bit0 = directory
        uint8_t  category    = 0;   // cs::Category
        uint64_t totalSize   = 0;   // subtree logical size (aggregate())
        uint64_t totalAlloc  = 0;   // subtree on-disk size (aggregate())
        uint32_t childCount  = 0;
        uint32_t recordIndex = 0;   // source MFT record (hard links share)
    };

    void clear();
    void reserve(size_t nodeCount, size_t nameChars);

    // Append a node, not yet linked into the hierarchy. Returns its index.
    uint32_t addNode(const wchar_t* name, uint32_t nameLength, uint64_t size,
                     uint64_t alloc, bool isDirectory, uint32_t recordIndex);

    // Insert `child` as a child of `parent` (order is arbitrary until sorted).
    void link(uint32_t parent, uint32_t child);

    void setRoot(uint32_t root) { root_ = root; }
    uint32_t root() const { return root_; }

    size_t      nodeCount() const { return nodes_.size(); }
    const Node& node(uint32_t i) const { return nodes_[i]; }
    Node&       node(uint32_t i) { return nodes_[i]; }
    bool        valid(uint32_t i) const { return i < nodes_.size(); }

    const wchar_t* name(uint32_t i) const { return names_.data() + nodes_[i].nameOffset; }
    std::wstring   nameString(uint32_t i) const;

    bool isDirectory(uint32_t i) const { return (nodes_[i].flags & 1) != 0; }

    // Roll subtree totals up from `root_`. Must be called after linking.
    void aggregate();

    // Reorder every node's children by descending totalSize.
    void sortChildrenBySizeDesc();

private:
    std::vector<Node>    nodes_;
    std::vector<wchar_t> names_;
    uint32_t             root_ = kInvalid;
};

}  // namespace cs
