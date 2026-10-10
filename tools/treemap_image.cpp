// Dev/QA tool: render a directory's treemap to a PNG using the same core layout
// + category logic as the GUI. Renders nested levels so file-type colors show.
// Lets you inspect the visualization without an elevated/GPU session.
//
//   treemap_image <directory> <out.png>

// GDI+ needs the OLE headers that WIN32_LEAN_AND_MEAN excludes.
#undef WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "format.hpp"
#include "scan.hpp"
#include "tree.hpp"
#include "treemap.hpp"
#include "utf8.hpp"

namespace {

Gdiplus::Color color_for(uint8_t category, bool is_dir) {
    if (is_dir) return Gdiplus::Color(255, 58, 66, 88);
    switch (static_cast<cs::Category>(category)) {
        case cs::Category::Video:      return Gdiplus::Color(255, 198, 72, 72);
        case cs::Category::Image:      return Gdiplus::Color(255, 214, 140, 58);
        case cs::Category::Audio:      return Gdiplus::Color(255, 178, 104, 186);
        case cs::Category::Document:   return Gdiplus::Color(255, 72, 118, 198);
        case cs::Category::Archive:    return Gdiplus::Color(255, 160, 156, 70);
        case cs::Category::Executable: return Gdiplus::Color(255, 88, 176, 116);
        case cs::Category::Code:       return Gdiplus::Color(255, 70, 166, 178);
        default:                       return Gdiplus::Color(255, 120, 122, 128);
    }
}

std::wstring widen(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

int get_encoder_clsid(const wchar_t* mime, CLSID* clsid) {
    UINT num = 0;
    UINT size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<uint8_t> buf(size);
    auto* info = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(info[i].MimeType, mime) == 0) {
            *clsid = info[i].Clsid;
            return static_cast<int>(i);
        }
    }
    return -1;
}

struct Ctx {
    const cs::VolumeTree* tree = nullptr;
    Gdiplus::Graphics* g = nullptr;
    Gdiplus::Font* font = nullptr;
    Gdiplus::Font* subfont = nullptr;
    Gdiplus::SolidBrush* white = nullptr;
    Gdiplus::Pen* border = nullptr;
    uint64_t root_total = 0;
    int max_depth = 4;
    size_t other_count = 0;
};

void draw_children(Ctx& c, uint32_t node, const cs::Rect& bounds, int depth) {
    const cs::VolumeTree& t = *c.tree;
    struct Item {
        uint32_t node;   // kInvalid => other bucket
        uint64_t size;
    };
    std::vector<Item> items;
    uint64_t other = 0;
    size_t other_n = 0;
    for (uint32_t ch = t.node(node).firstChild; ch != cs::VolumeTree::kInvalid;
         ch = t.node(ch).nextSibling) {
        const uint64_t w = t.node(ch).totalSize;
        if (w == 0) continue;
        // Only fold small items at the root (mirrors the GUI's first screen).
        if (depth == 0) {
            const uint64_t threshold = c.root_total > 1000 ? c.root_total / 1000 : 1024;
            if (w < threshold) {
                other += w;
                ++other_n;
                continue;
            }
        }
        items.push_back({ch, w});
    }
    if (other_n > 0) {
        c.other_count = other_n;
        items.push_back({cs::VolumeTree::kInvalid, other});
    }
    if (items.empty()) return;
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.size > b.size; });

    std::vector<double> weights;
    for (const auto& it : items) weights.push_back(static_cast<double>(it.size));
    std::vector<cs::Rect> rects;
    cs::squarified_treemap(weights, bounds, rects);

    for (size_t i = 0; i < rects.size(); ++i) {
        const cs::Rect r = rects[i];
        if (r.w < 1.0f || r.h < 1.0f) continue;
        const bool is_other = items[i].node == cs::VolumeTree::kInvalid;
        const bool is_dir = !is_other && t.isDirectory(items[i].node);

        Gdiplus::SolidBrush brush(is_other ? Gdiplus::Color(255, 70, 70, 74)
                                           : color_for(t.node(items[i].node).category, is_dir));
        c.g->FillRectangle(&brush, r.x, r.y, r.w, r.h);
        c.g->DrawRectangle(c.border, r.x, r.y, r.w, r.h);

        const float label_h = (r.h > 40.0f) ? 15.0f : 0.0f;
        if (r.w > 54.0f && r.h > 24.0f) {
            const std::wstring name =
                is_other ? (L"其他 (" + std::to_wstring(other_n) + L" 项)")
                         : t.nameString(items[i].node);
            const double pct = c.root_total > 0
                                   ? 100.0 * static_cast<double>(items[i].size) /
                                         static_cast<double>(c.root_total)
                                   : 0.0;
            c.g->SetClip(Gdiplus::RectF(r.x, r.y, r.w, r.h));
            c.g->DrawString(name.c_str(), -1, c.font, Gdiplus::PointF(r.x + 3, r.y + 1),
                            c.white);
            if (label_h > 0.0f && r.h > 40.0f) {
                wchar_t buf[64];
                std::swprintf(buf, 64, L"%s · %.1f%%",
                              widen(cs::human_size(items[i].size)).c_str(), pct);
                c.g->DrawString(buf, -1, c.subfont, Gdiplus::PointF(r.x + 3, r.y + 15),
                                c.white);
            }
            c.g->ResetClip();
        }

        if (is_dir && depth < c.max_depth) {
            const cs::Rect inner{r.x + 2.0f, r.y + 2.0f + label_h, r.w - 4.0f,
                                 r.h - 4.0f - label_h};
            if (inner.w > 6.0f && inner.h > 6.0f) draw_children(c, items[i].node, inner, depth + 1);
        }
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: treemap_image <directory> <out.png>\n");
        return 2;
    }
    const std::wstring dir = argv[1];
    const std::wstring out_path = argv[2];

    cs::VolumeTree tree;
    cs::ScanProgress progress;
    std::wstring error;
    if (!cs::scan_directory_recursive(dir, tree, progress, error)) {
        std::fprintf(stderr, "scan failed\n");
        return 1;
    }

    const float W = 1200.0f;
    const float H = 720.0f;
    const uint64_t root_total = tree.node(tree.root()).totalSize;

    Gdiplus::GdiplusStartupInput gdi_input;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &gdi_input, nullptr);
    {
        Gdiplus::Bitmap bmp(static_cast<INT>(W), static_cast<INT>(H), PixelFormat32bppARGB);
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        g.Clear(Gdiplus::Color(255, 17, 17, 18));

        Gdiplus::FontFamily family(L"Microsoft YaHei");
        Gdiplus::Font font(&family, 12.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font subfont(&family, 10.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush white(Gdiplus::Color(255, 240, 240, 245));
        Gdiplus::Pen border(Gdiplus::Color(255, 10, 10, 12), 1.0f);

        Ctx ctx;
        ctx.tree = &tree;
        ctx.g = &g;
        ctx.font = &font;
        ctx.subfont = &subfont;
        ctx.white = &white;
        ctx.border = &border;
        ctx.root_total = root_total;
        draw_children(ctx, tree.root(), cs::Rect{0, 0, W, H}, 0);

        CLSID png_clsid;
        if (get_encoder_clsid(L"image/png", &png_clsid) >= 0) {
            bmp.Save(out_path.c_str(), &png_clsid, nullptr);
            std::printf("wrote %s (total %s)\n", cs::wide_to_utf8(out_path).c_str(),
                        cs::human_size(root_total).c_str());
        }
    }
    Gdiplus::GdiplusShutdown(token);
    return 0;
}
