#include "treemap_view.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "format.hpp"
#include "imgui.h"
#include "utf8.hpp"

namespace gui {
namespace {

struct Item {
    uint32_t node;     // kInvalid => "other" bucket
    double   weight;
};

}  // namespace

uint32_t category_color(uint8_t category) {
    switch (static_cast<cs::Category>(category)) {
        case cs::Category::Video:      return IM_COL32(198, 72, 72, 235);
        case cs::Category::Image:      return IM_COL32(214, 140, 58, 235);
        case cs::Category::Audio:      return IM_COL32(178, 104, 186, 235);
        case cs::Category::Document:   return IM_COL32(72, 118, 198, 235);
        case cs::Category::Archive:    return IM_COL32(160, 156, 70, 235);
        case cs::Category::Executable: return IM_COL32(88, 176, 116, 235);
        case cs::Category::Code:       return IM_COL32(70, 166, 178, 235);
        default:                       return IM_COL32(120, 122, 128, 235);
    }
}

const char* category_name(uint8_t category) {
    switch (static_cast<cs::Category>(category)) {
        case cs::Category::Video:      return "视频";
        case cs::Category::Image:      return "图片";
        case cs::Category::Audio:      return "音频";
        case cs::Category::Document:   return "文档";
        case cs::Category::Archive:    return "压缩包";
        case cs::Category::Executable: return "可执行";
        case cs::Category::Code:       return "代码";
        default:                       return "其他";
    }
}

TreemapResult draw_treemap(ImDrawList* draw, const cs::VolumeTree& tree, uint32_t node,
                           const cs::Rect& bounds, uint64_t threshold, SizeBasis basis,
                           bool windowHovered) {
    TreemapResult result;
    if (!tree.valid(node) || bounds.w <= 1.0f || bounds.h <= 1.0f) return result;

    const auto& parent = tree.node(node);
    std::vector<Item> items;
    items.reserve(parent.childCount + 1);

    for (uint32_t c = parent.firstChild; c != cs::VolumeTree::kInvalid;
         c = tree.node(c).nextSibling) {
        const auto& n = tree.node(c);
        const uint64_t w = (basis == SizeBasis::Logical) ? n.totalSize : n.totalAlloc;
        if (n.totalSize == 0 && n.totalAlloc == 0) continue;  // empty dir / zero file
        if (w < threshold) {
            result.otherSize += w;
            result.otherNodes.push_back(c);
            continue;
        }
        items.push_back({c, static_cast<double>(w)});
    }

    if (!result.otherNodes.empty()) {
        items.push_back({cs::VolumeTree::kInvalid, static_cast<double>(result.otherSize)});
    }
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.weight > b.weight; });

    std::vector<double> weights;
    weights.reserve(items.size());
    for (const auto& it : items) weights.push_back(it.weight);

    std::vector<cs::Rect> rects;
    cs::squarified_treemap(weights, bounds, rects);

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool can_hover = windowHovered;

    for (size_t i = 0; i < rects.size(); ++i) {
        const cs::Rect r = rects[i];
        if (r.w < 0.5f || r.h < 0.5f) continue;
        const ImVec2 p0(r.x, r.y);
        const ImVec2 p1(r.x + r.w, r.y + r.h);

        uint32_t fill;
        if (items[i].node == cs::VolumeTree::kInvalid) {
            fill = IM_COL32(70, 70, 74, 235);
        } else if (tree.isDirectory(items[i].node)) {
            fill = IM_COL32(58, 66, 88, 235);
        } else {
            fill = category_color(tree.node(items[i].node).category);
        }
        draw->AddRectFilled(p0, p1, fill);
        draw->AddRect(p0, p1, IM_COL32(20, 20, 22, 255));

        if (r.w > 46.0f && r.h > 16.0f) {
            const std::string label =
                items[i].node == cs::VolumeTree::kInvalid
                    ? ("其他 (" + std::to_string(result.otherNodes.size()) + ")")
                    : std::string();
            std::string text = label;
            if (text.empty()) {
                text = cs::wide_to_utf8(tree.nameString(items[i].node));
            }
            draw->PushClipRect(p0, p1, true);
            draw->AddText(ImVec2(r.x + 4.0f, r.y + 3.0f), IM_COL32(240, 240, 245, 255),
                          text.c_str());
            draw->PopClipRect();
        }

        if (can_hover && mouse.x >= r.x && mouse.x < r.x + r.w && mouse.y >= r.y &&
            mouse.y < r.y + r.h) {
            result.hoveredNode = static_cast<int32_t>(i);
            result.hoveredTree = items[i].node;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (items[i].node == cs::VolumeTree::kInvalid) {
                    result.otherClicked = true;
                } else {
                    result.clickedRect = static_cast<int32_t>(i);
                    result.clickedTree = items[i].node;
                }
            }
        }
    }

    if (can_hover && result.hoveredTree != cs::VolumeTree::kInvalid) {
        const auto& n = tree.node(result.hoveredTree);
        const std::string utf8 = cs::wide_to_utf8(tree.nameString(result.hoveredTree));
        const uint64_t shown = (basis == SizeBasis::Logical) ? n.totalSize : n.totalAlloc;
        const double pct = parent.totalSize > 0
                               ? 100.0 * static_cast<double>(n.totalSize) /
                                     static_cast<double>(parent.totalSize)
                               : 0.0;
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(utf8.c_str());
        ImGui::Text("%s", tree.isDirectory(result.hoveredTree) ? "文件夹" : "文件");
        ImGui::Text("大小: %s", cs::human_size(shown).c_str());
        ImGui::Text("占比: %.2f%%", pct);
        ImGui::EndTooltip();
    }

    return result;
}

}  // namespace gui
