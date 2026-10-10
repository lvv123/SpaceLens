#pragma once

// Treemap rendering. Draws the current node's children and (nested) their
// children too, so file-type colors show without drilling in. Reports hover and
// click for the deepest tile under the cursor.

#include <cstdint>
#include <vector>

#include "tree.hpp"
#include "treemap.hpp"

struct ImDrawList;

namespace gui {

enum class SizeBasis : int { Logical = 0, OnDisk = 1 };

uint32_t category_color(uint8_t category);
const char* category_name(uint8_t category);

struct TreemapResult {
    int32_t  hoveredNode  = -1;   // index into the drawn order, or -1
    uint32_t hoveredTree  = cs::VolumeTree::kInvalid;
    int32_t  clickedRect  = -1;   // index of the clicked rectangle, or -1
    uint32_t clickedTree  = cs::VolumeTree::kInvalid;
    bool     otherClicked = false;
    std::vector<uint32_t> otherNodes;  // siblings folded into the "other" bucket
    uint64_t otherSize = 0;
};

TreemapResult draw_treemap(ImDrawList* draw, const cs::VolumeTree& tree, uint32_t node,
                           const cs::Rect& bounds, uint64_t threshold, SizeBasis basis,
                           bool windowHovered);

}  // namespace gui
