#include "treemap_view.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "format.hpp"
#include "imgui.h"
#include "utf8.hpp"

namespace gui {
namespace {

constexpr int kMaxNestedLevels = 1;  // 1 => draw children and grandchildren

struct Item {
    uint32_t node;  // kInvalid => "other" bucket
    double   weight;
};

struct DrawCtx {
    ImDrawList*         draw = nullptr;
    const cs::VolumeTree* tree = nullptr;
    uint64_t            threshold = 0;
    SizeBasis           basis = SizeBasis::Logical;
    ImVec2              mouse{};
    bool                canHover = false;
    uint64_t            topTotal = 0;

    // outputs
    bool       haveHover = false;
    cs::Rect   hoverRect{};
    uint32_t   hoverTree = cs::VolumeTree::kInvalid;
    bool       haveClick = false;
    uint32_t   clickTree = cs::VolumeTree::kInvalid;
    bool       otherClicked = false;
    std::vector<uint32_t> otherNodes;
};

ImU32 brighten(ImU32 col, int d) {
    const int a = static_cast<int>((col >> IM_COL32_A_SHIFT) & 0xFF);
    int r = static_cast<int>((col >> IM_COL32_R_SHIFT) & 0xFF) + d;
    int g = static_cast<int>((col >> IM_COL32_G_SHIFT) & 0xFF) + d;
    int b = static_cast<int>((col >> IM_COL32_B_SHIFT) & 0xFF) + d;
    r = (r > 255) ? 255 : r;
    g = (g > 255) ? 255 : g;
    b = (b > 255) ? 255 : b;
    return IM_COL32(r, g, b, a);
}

// Truncate a UTF-8 string with an ellipsis so it fits within maxW pixels.
std::string ellipsize(const std::string& s, float maxW) {
    if (ImGui::CalcTextSize(s.c_str()).x <= maxW) return s;
    std::string t = s;
    while (!t.empty()) {
        size_t i = t.size();
        while (i > 0 && (static_cast<unsigned char>(t[i - 1]) & 0xC0) == 0x80) --i;
        if (i > 0) --i;
        t.erase(i);
        const std::string cand = t + "…";
        if (ImGui::CalcTextSize(cand.c_str()).x <= maxW) return cand;
    }
    return std::string();
}

void draw_level(DrawCtx& ctx, uint32_t parent, const cs::Rect& bounds, int depth) {
    const cs::VolumeTree& tree = *ctx.tree;
    if (!tree.valid(parent) || bounds.w <= 1.0f || bounds.h <= 1.0f) return;

    std::vector<Item> items;
    uint64_t otherSize = 0;
    std::vector<uint32_t> otherNodes;
    for (uint32_t c = tree.node(parent).firstChild; c != cs::VolumeTree::kInvalid;
         c = tree.node(c).nextSibling) {
        const auto& n = tree.node(c);
        const uint64_t w = (ctx.basis == SizeBasis::Logical) ? n.totalSize : n.totalAlloc;
        if (n.totalSize == 0 && n.totalAlloc == 0) continue;
        if (w < ctx.threshold) {
            otherSize += w;
            otherNodes.push_back(c);
            continue;
        }
        items.push_back({c, static_cast<double>(w)});
    }
    if (!otherNodes.empty()) items.push_back({cs::VolumeTree::kInvalid, static_cast<double>(otherSize)});
    if (items.empty()) return;
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.weight > b.weight; });

    std::vector<double> weights;
    weights.reserve(items.size());
    for (const auto& it : items) weights.push_back(it.weight);
    std::vector<cs::Rect> rects;
    cs::squarified_treemap(weights, bounds, rects);

    for (size_t i = 0; i < rects.size(); ++i) {
        const cs::Rect r = rects[i];
        if (r.w < 0.5f || r.h < 0.5f) continue;
        const ImVec2 p0(r.x, r.y);
        const ImVec2 p1(r.x + r.w, r.y + r.h);
        const bool isOther = items[i].node == cs::VolumeTree::kInvalid;
        const bool isDir = !isOther && tree.isDirectory(items[i].node);

        ImU32 fill;
        if (isOther) {
            fill = IM_COL32(74, 76, 82, 255);
        } else if (isDir) {
            fill = (depth == 0) ? IM_COL32(60, 70, 96, 255) : IM_COL32(52, 60, 82, 255);
        } else {
            fill = category_color(tree.node(items[i].node).category);
        }
        ctx.draw->AddRectFilled(p0, p1, fill, 3.0f);
        ctx.draw->AddRect(p0, p1, IM_COL32(14, 15, 19, 200), 3.0f, 0, 1.0f);

        const bool labeled = (r.w > 46.0f && r.h > 16.0f);
        if (labeled) {
            std::string text;
            if (isOther)
                text = "其他 (" + std::to_string(otherNodes.size()) + ")";
            else
                text = cs::wide_to_utf8(tree.nameString(items[i].node));
            text = ellipsize(text, r.w - 9.0f);
            if (!text.empty()) {
                ctx.draw->PushClipRect(p0, p1, true);
                ctx.draw->AddText(ImVec2(r.x + 5.0f, r.y + 4.0f), IM_COL32(0, 0, 0, 170),
                                  text.c_str());
                ctx.draw->AddText(ImVec2(r.x + 4.0f, r.y + 3.0f), IM_COL32(245, 246, 250, 255),
                                  text.c_str());
                ctx.draw->PopClipRect();
            }
        }

        if (ctx.canHover && ctx.mouse.x >= r.x && ctx.mouse.x < r.x + r.w &&
            ctx.mouse.y >= r.y && ctx.mouse.y < r.y + r.h) {
            ctx.haveHover = true;  // children drawn later override -> deepest wins
            ctx.hoverRect = r;
            ctx.hoverTree = items[i].node;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (isOther) {
                    ctx.otherClicked = true;
                    ctx.otherNodes = otherNodes;
                } else {
                    ctx.haveClick = true;
                    ctx.clickTree = items[i].node;
                }
            }
        }

        if (isDir && depth < kMaxNestedLevels) {
            const float labelH = labeled ? (ImGui::GetTextLineHeight() + 5.0f) : 4.0f;
            const cs::Rect inner{r.x + 3.0f, r.y + 3.0f + labelH, r.w - 6.0f,
                                 r.h - 6.0f - labelH};
            if (inner.w > 8.0f && inner.h > 8.0f)
                draw_level(ctx, items[i].node, inner, depth + 1);
        }
    }
}

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

    DrawCtx ctx;
    ctx.draw = draw;
    ctx.tree = &tree;
    ctx.threshold = threshold;
    ctx.basis = basis;
    ctx.mouse = ImGui::GetIO().MousePos;
    ctx.canHover = windowHovered;
    ctx.topTotal = tree.node(node).totalSize;

    draw_level(ctx, node, bounds, 0);

    if (ctx.haveHover) {
        const ImVec2 p0(ctx.hoverRect.x, ctx.hoverRect.y);
        const ImVec2 p1(ctx.hoverRect.x + ctx.hoverRect.w, ctx.hoverRect.y + ctx.hoverRect.h);
        draw->AddRect(p0, p1, IM_COL32(150, 200, 255, 255), 3.0f, 0, 2.0f);
    }

    result.otherClicked = ctx.otherClicked;
    result.otherNodes = ctx.otherNodes;
    result.clickedTree = ctx.clickTree;
    result.hoveredTree = ctx.hoverTree;

    if (ctx.canHover && ctx.hoverTree != cs::VolumeTree::kInvalid) {
        const auto& n = tree.node(ctx.hoverTree);
        const std::string name = cs::wide_to_utf8(tree.nameString(ctx.hoverTree));
        const uint64_t shown = (basis == SizeBasis::Logical) ? n.totalSize : n.totalAlloc;
        const double pct = ctx.topTotal > 0 ? 100.0 * static_cast<double>(n.totalSize) /
                                                  static_cast<double>(ctx.topTotal)
                                            : 0.0;
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(name.c_str());
        ImGui::TextDisabled("%s", tree.isDirectory(ctx.hoverTree) ? "文件夹" : "文件");
        ImGui::Text("大小: %s", cs::human_size(shown).c_str());
        ImGui::Text("占比: %.2f%%", pct);
        ImGui::EndTooltip();
    }

    return result;
}

}  // namespace gui
