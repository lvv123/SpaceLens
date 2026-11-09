#include "app.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <future>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "format.hpp"
#include "scan.hpp"
#include "tree.hpp"
#include "utf8.hpp"

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace gui {
namespace {

enum class SizeBasis : int { Logical = 0, OnDisk = 1 };

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

// Optional debug log: set SPACELENS_LOG to a file path.
void dbg(const char* msg) {
    static FILE* f = nullptr;
    static bool initialized = false;
    if (!initialized) {
        initialized = true;
        wchar_t path[1024] = {};
        if (GetEnvironmentVariableW(L"SPACELENS_LOG", path, 1024) > 0) {
            _wfopen_s(&f, path, L"a");
        }
    }
    if (f != nullptr) {
        std::fprintf(f, "%s\n", msg);
        std::fflush(f);
    }
}

// ---------------------------------------------------------------- Direct3D 11
ID3D11Device*            g_device = nullptr;
ID3D11DeviceContext*     g_context = nullptr;
IDXGISwapChain*          g_swapChain = nullptr;
ID3D11RenderTargetView*  g_rtv = nullptr;
bool                     g_swapOccluded = false;
UINT                     g_resizeW = 0;
UINT                     g_resizeH = 0;

bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
        &g_swapChain, &g_device, &level, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device,
                                           &level, &g_context);
    }
    return SUCCEEDED(hr);
}

void cleanup_rtv();  // 定义在 create_rtv 之后

bool create_rtv() {
    // 中文注释：创建失败必须返回 false；否则后续会用空 RTV 清屏，DX11 会报错。
    cleanup_rtv();
    ID3D11Texture2D* back = nullptr;
    const HRESULT getHr = g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (FAILED(getHr) || back == nullptr) return false;
    const HRESULT rtvHr = g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
    if (FAILED(rtvHr)) {
        g_rtv = nullptr;
        return false;
    }
    return g_rtv != nullptr;
}

void cleanup_rtv() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

void cleanup_device() {
    cleanup_rtv();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context)   { g_context->Release();   g_context = nullptr; }
    if (g_device)    { g_device->Release();    g_device = nullptr; }
}

// Dev/QA: dump the current back buffer to a 32-bit BMP (env SPACELENS_CAPTURE).
void save_backbuffer_bmp(const std::wstring& path) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) || back == nullptr) return;
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&sd, nullptr, &staging)) && staging != nullptr) {
        g_context->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            const uint32_t w = desc.Width;
            const uint32_t h = desc.Height;
            FILE* f = nullptr;
            _wfopen_s(&f, path.c_str(), L"wb");
            if (f != nullptr) {
                const uint32_t row_bytes = w * 4;
                const uint32_t image_size = row_bytes * h;
                const uint32_t file_size = 54 + image_size;
                uint8_t hdr[54] = {0};
                hdr[0] = 'B'; hdr[1] = 'M';
                std::memcpy(hdr + 2, &file_size, 4);
                const uint32_t off = 54; std::memcpy(hdr + 10, &off, 4);
                const uint32_t dib = 40; std::memcpy(hdr + 14, &dib, 4);
                const int32_t ww = static_cast<int32_t>(w); std::memcpy(hdr + 18, &ww, 4);
                const int32_t hh = -static_cast<int32_t>(h); std::memcpy(hdr + 22, &hh, 4);
                const uint16_t planes = 1; std::memcpy(hdr + 26, &planes, 2);
                const uint16_t bpp = 32; std::memcpy(hdr + 28, &bpp, 2);
                std::fwrite(hdr, 1, 54, f);
                std::vector<uint8_t> row(row_bytes);
                for (uint32_t y = 0; y < h; ++y) {
                    const uint8_t* src =
                        static_cast<const uint8_t*>(mapped.pData) +
                        static_cast<size_t>(y) * mapped.RowPitch;
                    for (uint32_t x = 0; x < w; ++x) {  // RGBA -> BGRA
                        row[x * 4 + 0] = src[x * 4 + 2];
                        row[x * 4 + 1] = src[x * 4 + 1];
                        row[x * 4 + 2] = src[x * 4 + 0];
                        row[x * 4 + 3] = 255;
                    }
                    std::fwrite(row.data(), 1, row_bytes, f);
                }
                std::fclose(f);
            }
            g_context->Unmap(staging, 0);
        }
        staging->Release();
    }
    back->Release();
}

// --------------------------------------------------------------------- State
// 中文注释：工作线程只构造这个局部结果，不直接触碰下面的 g_tree/g_error。
// UI 线程在 poll_scan() 中取回结果后再一次性安装，避免数据竞争。
struct ScanOutcome {
    bool ok = false;
    cs::VolumeTree tree;
    std::wstring error;
};

cs::VolumeTree            g_tree;
std::vector<std::wstring> g_volumes;
int                       g_volumeIndex = 0;
std::future<ScanOutcome>  g_scanFuture;
cs::ScanProgress          g_progress;
std::atomic<bool>         g_scanning{false};
std::vector<uint32_t>     g_path;
uint32_t                  g_current = cs::VolumeTree::kInvalid;
uint64_t                  g_threshold = 1u << 20;
SizeBasis                 g_basis = SizeBasis::Logical;
std::wstring              g_error;
uint64_t                  g_filteredCount = 0;
uint64_t                  g_filteredBytes = 0;
bool                      g_volumeAccessible = true;
std::wstring              g_capturePath;  // dev: SPACELENS_CAPTURE

std::vector<uint32_t> g_childOrder;      // current folder's children, sorted/filtered
int                   g_sortCol = 1;     // 0 = name, 1 = size, 2 = percent
bool                  g_sortAscending = false;
bool                  g_orderDirty = true;
char                  g_filter[96] = "";

LRESULT WINAPI wnd_proc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            g_resizeW = static_cast<UINT>(LOWORD(lParam));
            g_resizeH = static_cast<UINT>(HIWORD(lParam));
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // disable ALT menu
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool can_open_volume() {
    HANDLE h = CreateFileW(L"\\\\.\\C:", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

void reset_progress() {
    // 中文注释：统一交给 ScanProgress::reset()，避免新增计数字段时漏清零。
    g_progress.reset();
}

// Dev-only: when SPACELENS_DEV_DIR is set, scan that directory with the
// portable recursive scanner instead of MFT. Lets the GUI be exercised without
// administrator rights. Never set in normal runs.
bool dev_recursive_dir(std::wstring& out) {
    wchar_t buf[1024] = {};
    const DWORD n = GetEnvironmentVariableW(L"SPACELENS_DEV_DIR", buf, 1024);
    if (n == 0 || n >= 1024) return false;
    out.assign(buf, n);
    return true;
}

void start_scan() {
    // 中文注释：正常流程下扫描按钮在扫描期间是禁用的；这里仍防御性收尾上一次
    // 任务，保证 std::future 在复用前已经取回结果。
    if (g_scanFuture.valid()) {
        g_progress.cancel.store(true, std::memory_order_relaxed);
        try {
            g_scanFuture.get();
        } catch (...) {
        }
    }
    g_tree.clear();
    g_path.clear();
    g_current = cs::VolumeTree::kInvalid;
    g_error.clear();
    reset_progress();
    g_scanning.store(true);

    std::wstring devDir;
    if (dev_recursive_dir(devDir)) {
        try {
            g_scanFuture = std::async(std::launch::async, [devDir]() {
                ScanOutcome out;
                out.ok = cs::scan_directory_recursive(devDir, out.tree, g_progress, out.error);
                return out;
            });
        } catch (const std::exception&) {
            g_scanning.store(false);
            g_error = L"无法启动扫描线程";
            g_progress.failed.store(true, std::memory_order_relaxed);
        }
        return;
    }

    const std::wstring vol = g_volumes.empty() ? std::wstring(L"C:") : g_volumes[g_volumeIndex];
    try {
        g_scanFuture = std::async(std::launch::async, [vol]() {
            ScanOutcome out;
            out.ok = cs::scan_volume_mft(vol, out.tree, g_progress, out.error);
            return out;
        });
    } catch (const std::exception&) {
        g_scanning.store(false);
        g_error = L"无法启动扫描线程";
        g_progress.failed.store(true, std::memory_order_relaxed);
    }
}

void poll_scan() {
    if (!g_scanning.load()) return;
    if (!g_progress.finished.load() && !g_progress.failed.load()) return;

    // 中文注释：扫描线程只写自己的局部结果；在这里 get() 之后才把树交给 UI，
    // 因此之后的绘制/日志读取都不会与工作线程并发访问同一棵树。
    ScanOutcome out;
    if (g_scanFuture.valid()) {
        try {
            out = g_scanFuture.get();
        } catch (const std::bad_alloc&) {
            out.ok = false;
            out.error = L"内存不足，扫描已中止";
        } catch (const std::exception&) {
            out.ok = false;
            out.error = L"扫描线程发生异常";
        }
    } else {
        out.error = L"扫描任务状态异常";
    }
    g_scanning.store(false);

    if (out.ok && out.tree.nodeCount() > 0) {
        g_tree = std::move(out.tree);
        g_current = g_tree.root();
        g_path.assign(1, g_current);
        g_orderDirty = true;
        const uint64_t rs = g_tree.node(g_current).totalSize;
        g_threshold = (rs > 1000) ? rs / 1000 : 1024;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "scan: records=%llu files=%llu dirs=%llu orphans=%llu (%llu bytes) "
                      "noParent=%llu seqMismatch=%llu",
                      static_cast<unsigned long long>(g_progress.totalRecords.load()),
                      static_cast<unsigned long long>(g_progress.fileCount.load()),
                      static_cast<unsigned long long>(g_progress.dirCount.load()),
                      static_cast<unsigned long long>(g_progress.orphanNodes.load()),
                      static_cast<unsigned long long>(g_progress.orphanBytes.load()),
                      static_cast<unsigned long long>(g_progress.orphanNoParent.load()),
                      static_cast<unsigned long long>(g_progress.orphanSeqMismatch.load()));
        dbg(buf);
    } else {
        g_error = out.error.empty() ? L"扫描失败" : out.error;
        g_progress.failed.store(true, std::memory_order_relaxed);
    }
}

void navigate_to(uint32_t node) {
    if (!g_tree.valid(node)) return;
    g_current = node;
    g_path.push_back(node);
    g_orderDirty = true;
}

void navigate_up() {
    if (g_path.size() > 1) {
        g_path.pop_back();
        g_current = g_path.back();
        g_orderDirty = true;
    }
}

std::string ascii_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Full filesystem path of a node (root name is like "C:").
std::wstring build_path(uint32_t node) {
    std::vector<uint32_t> chain;
    for (uint32_t n = node; n != cs::VolumeTree::kInvalid; n = g_tree.node(n).parent)
        chain.push_back(n);
    std::wstring path;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const std::wstring name = g_tree.nameString(*it);
        if (path.empty()) {
            path = name;
        } else {
            if (path.back() != L'\\') path += L'\\';
            path += name;
        }
    }
    return path;
}

void open_in_explorer(uint32_t node) {
    const std::wstring path = build_path(node);
    const bool isDir = g_tree.isDirectory(node);
    const std::wstring params = isDir ? (L"\"" + path + L"\"")
                                      : (L"/select,\"" + path + L"\"");
    ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
}

uint64_t node_size(uint32_t i) {
    return (g_basis == SizeBasis::Logical) ? g_tree.node(i).totalSize
                                           : g_tree.node(i).totalAlloc;
}

void sort_children_by_spec() {
    std::sort(g_childOrder.begin(), g_childOrder.end(), [](uint32_t a, uint32_t b) {
        // 中文注释：比较器必须满足严格弱序。相等时不能返回 !less（那会同时
        // 认为 a<b 和 b<a），而应退回稳定的索引比较。
        bool aLess = false;
        bool bLess = false;
        if (g_sortCol == 0) {
            const std::wstring an = g_tree.nameString(a);
            const std::wstring bn = g_tree.nameString(b);
            aLess = an < bn;
            bLess = bn < an;
        } else {
            const uint64_t as = node_size(a);
            const uint64_t bs = node_size(b);
            aLess = as < bs;
            bLess = bs < as;
        }
        if (aLess) return g_sortAscending;
        if (bLess) return !g_sortAscending;
        return a < b;
    });
}

void refresh_children() {
    g_childOrder.clear();
    g_orderDirty = false;
    g_filteredCount = 0;
    g_filteredBytes = 0;
    if (!g_tree.valid(g_current)) return;
    const std::string filter = ascii_lower(g_filter);
    for (uint32_t c = g_tree.node(g_current).firstChild; c != cs::VolumeTree::kInvalid;
         c = g_tree.node(c).nextSibling) {
        if (!filter.empty()) {
            const std::string nm = ascii_lower(cs::wide_to_utf8(g_tree.nameString(c)));
            if (nm.find(filter) == std::string::npos) continue;
        }
        const uint64_t sz = node_size(c);
        if (sz > 0 && sz < g_threshold) {
            ++g_filteredCount;
            g_filteredBytes += sz;
            continue;
        }
        g_childOrder.push_back(c);
    }
    sort_children_by_spec();
}

void setup_style(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 8.0f;
    s.FrameRounding = 6.0f;
    s.PopupRounding = 8.0f;
    s.GrabRounding = 6.0f;
    s.ScrollbarRounding = 8.0f;
    s.TabRounding = 6.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.PopupBorderSize = 1.0f;
    s.WindowPadding = ImVec2(14.0f, 12.0f);
    s.FramePadding = ImVec2(10.0f, 6.0f);
    s.CellPadding = ImVec2(8.0f, 6.0f);
    s.ItemSpacing = ImVec2(10.0f, 8.0f);
    s.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    s.ScrollbarSize = 13.0f;
    s.GrabMinSize = 12.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]             = ImVec4(0.086f, 0.094f, 0.113f, 1.00f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.117f, 0.129f, 0.157f, 1.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.109f, 0.121f, 0.149f, 0.98f);
    c[ImGuiCol_Border]               = ImVec4(0.235f, 0.259f, 0.310f, 0.70f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.160f, 0.180f, 0.220f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.220f, 0.250f, 0.310f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.260f, 0.300f, 0.370f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.070f, 0.078f, 0.098f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.098f, 0.110f, 0.137f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.204f, 0.420f, 0.720f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.263f, 0.502f, 0.820f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.173f, 0.361f, 0.630f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.220f, 0.300f, 0.440f, 0.80f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.278f, 0.380f, 0.550f, 0.90f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.302f, 0.420f, 0.600f, 1.00f);
    c[ImGuiCol_Separator]            = ImVec4(0.235f, 0.259f, 0.310f, 0.60f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.349f, 0.620f, 0.950f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.450f, 0.700f, 1.000f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.450f, 0.700f, 1.000f, 1.00f);
    c[ImGuiCol_Text]                 = ImVec4(0.920f, 0.930f, 0.960f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.500f, 0.520f, 0.580f, 1.00f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.098f, 0.110f, 0.137f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.278f, 0.310f, 0.380f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.350f, 0.390f, 0.470f, 1.00f);
    c[ImGuiCol_TableHeaderBg]        = ImVec4(0.149f, 0.170f, 0.216f, 1.00f);
    c[ImGuiCol_TableRowBg]           = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(1.0f, 1.0f, 1.0f, 0.025f);

    s.ScaleAllSizes(scale);
}

void load_font(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    const float size = 18.0f * scale;
    const char* candidates[] = {"C:\\Windows\\Fonts\\msyh.ttc",
                                "C:\\Windows\\Fonts\\msyh.ttf",
                                "C:\\Windows\\Fonts\\simhei.ttf",
                                "C:\\Windows\\Fonts\\simsun.ttc"};
    for (const char* path : candidates) {
        if (io.Fonts->AddFontFromFileTTF(path, size, nullptr,
                                         io.Fonts->GetGlyphRangesChineseSimplifiedCommon()) !=
            nullptr)
            return;
    }
    io.Fonts->AddFontDefault();
}

void draw_toolbar() {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float barH = ImGui::GetFrameHeight() + st.WindowPadding.y * 2.0f + 2.0f;
    ImGui::BeginChild("##toolbar", ImVec2(0, barH), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar);

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("卷");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    const std::string currentVol =
        g_volumes.empty() ? std::string("(无)") : cs::wide_to_utf8(g_volumes[g_volumeIndex]);
    if (ImGui::BeginCombo("##vol", currentVol.c_str())) {
        for (int i = 0; i < static_cast<int>(g_volumes.size()); ++i) {
            const std::string label = cs::wide_to_utf8(g_volumes[i]);
            if (ImGui::Selectable(label.c_str(), i == g_volumeIndex)) g_volumeIndex = i;
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine(0, 18);
    ImGui::BeginDisabled(g_scanning.load());
    if (ImGui::Button("  扫描  ")) start_scan();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_scanning.load());
    if (ImGui::Button("取消")) g_progress.cancel.store(true);
    ImGui::EndDisabled();

    ImGui::SameLine(0, 26);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("阈值");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    float logv =
        (g_threshold > 0) ? static_cast<float>(std::log10(static_cast<double>(g_threshold))) : 3.0f;
    if (logv < 3.0f) logv = 3.0f;
    if (ImGui::SliderFloat("##thresh", &logv, 3.0f, 10.5f, "")) {
        g_threshold = static_cast<uint64_t>(std::pow(10.0, static_cast<double>(logv)));
        if (g_threshold < 1024) g_threshold = 1024;
        g_orderDirty = true;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", cs::human_size(g_threshold).c_str());

    ImGui::SameLine(0, 26);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("口径");
    ImGui::SameLine();
    int basis = static_cast<int>(g_basis);
    if (ImGui::RadioButton("逻辑", basis == 0)) basis = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("占用", basis == 1)) basis = 1;
    if (static_cast<int>(g_basis) != basis) g_orderDirty = true;
    g_basis = static_cast<SizeBasis>(basis);

    ImGui::EndChild();
}

void header_cell(const char* label, int col) {
    ImGui::TableSetColumnIndex(col);
    const bool active = (g_sortCol == col);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s%s", label, active ? (g_sortAscending ? " ^" : " v") : "");
    ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(0.75f, 0.85f, 1.0f, 1.0f)
                                                 : ImVec4(0.62f, 0.66f, 0.74f, 1.0f));
    if (ImGui::Selectable(buf, false, ImGuiSelectableFlags_None)) {
        if (g_sortCol == col)
            g_sortAscending = !g_sortAscending;
        else {
            g_sortCol = col;
            g_sortAscending = (col == 0);
        }
        sort_children_by_spec();
    }
    ImGui::PopStyleColor();
}

// 中文注释：把单行绘制从 draw_list_panel 拆出来，降低主列表函数的复杂度。
void draw_child_row(uint32_t c, uint64_t total, ImDrawList* dl, float cellPad) {
    const auto& n = g_tree.node(c);
    const uint64_t sz = node_size(c);
    const double pct =
        total > 0 ? 100.0 * static_cast<double>(sz) / static_cast<double>(total) : 0.0;

    ImGui::TableNextRow();
    const ImVec2 rp = ImGui::GetCursorScreenPos();
    const float rw = ImGui::GetContentRegionAvail().x;
    const float rh = ImGui::GetTextLineHeight() + cellPad * 2.0f;
    const ImU32 barCol = g_tree.isDirectory(c) ? IM_COL32(96, 130, 190, 46)
                                               : IM_COL32(130, 170, 210, 38);
    dl->AddRectFilled(rp,
                      ImVec2(rp.x + static_cast<float>(rw * pct / 100.0), rp.y + rh),
                      barCol, 3.0f);

    ImGui::TableSetColumnIndex(0);
    const ImVec2 sp = ImGui::GetCursorScreenPos();
    const float ty = ImGui::GetTextLineHeight();
    const ImU32 sw = g_tree.isDirectory(c) ? IM_COL32(88, 108, 150, 255)
                                           : category_color(n.category);
    dl->AddRectFilled(ImVec2(sp.x, sp.y + (ty - 9.0f) * 0.5f),
                      ImVec2(sp.x + 9.0f, sp.y + (ty + 9.0f) * 0.5f), sw, 2.0f);
    ImGui::Dummy(ImVec2(15.0f, 0.0f));
    ImGui::SameLine(0.0f, 0.0f);

    const std::string name = cs::wide_to_utf8(g_tree.nameString(c));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.24f, 0.34f, 0.50f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.28f, 0.40f, 0.58f, 0.70f));
    ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_AllowOverlap);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\n%s", name.c_str(), cs::human_size(sz).c_str());
    if (ImGui::IsItemClicked() && g_tree.isDirectory(c) && n.childCount > 0) navigate_to(c);
    ImGui::PopStyleColor(3);

    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("在资源管理器中打开")) open_in_explorer(c);
        const std::string path = cs::wide_to_utf8(build_path(c));
        if (ImGui::MenuItem("复制路径")) ImGui::SetClipboardText(path.c_str());
        if (ImGui::MenuItem("复制大小"))
            ImGui::SetClipboardText(cs::human_size(sz).c_str());
        ImGui::EndPopup();
    }

    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(cs::human_size(sz).c_str());
    ImGui::TableSetColumnIndex(2);
    ImGui::Text("%.1f%%", pct);
}

void draw_list_panel(float width, float height) {
    ImGui::BeginChild("list", ImVec2(width, height), ImGuiChildFlags_Borders);

    if (g_scanning.load()) {
        const uint64_t total = g_progress.totalRecords.load();
        const uint64_t read = g_progress.recordsRead.load();
        const float frac =
            (total > 0) ? static_cast<float>(static_cast<double>(read) / total) : 0.0f;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%llu / %llu", static_cast<unsigned long long>(read),
                      static_cast<unsigned long long>(total));
        ImGui::TextUnformatted("正在扫描 MFT...");
        ImGui::ProgressBar(frac, ImVec2(-1, 0), buf);
        ImGui::Text("已读取 %s", cs::human_size(g_progress.bytesRead.load()).c_str());
        ImGui::EndChild();
        return;
    }

    if (g_path.size() > 1) {
        if (ImGui::Button("↑")) navigate_up();
    } else {
        ImGui::BeginDisabled(true);
        ImGui::Button("↑");
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    for (size_t i = 0; i < g_path.size(); ++i) {
        if (i > 0) {
            ImGui::SameLine(0, 6);
            ImGui::TextDisabled("/");
            ImGui::SameLine(0, 6);
        }
        const bool last = (i + 1 == g_path.size());
        std::string seg = cs::wide_to_utf8(g_tree.nameString(g_path[i]));
        if (seg.empty()) seg = "C:";
        if (last) {
            ImGui::TextUnformatted(seg.c_str());
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.72f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.22f, 0.30f, 0.44f, 1.0f));
            if (ImGui::Selectable(seg.c_str(), false, ImGuiSelectableFlags_None,
                                  ImVec2(ImGui::CalcTextSize(seg.c_str()).x, 0.0f))) {
                g_path.resize(i + 1);
                g_current = g_path.back();
                g_orderDirty = true;  // 中文注释：面包屑跳转后必须重建子项列表。
            }
            ImGui::PopStyleColor(2);
        }
    }
    ImGui::Separator();

    if (g_current == cs::VolumeTree::kInvalid) {
        if (!g_error.empty()) {
            ImGui::TextWrapped("扫描失败：%s", cs::wide_to_utf8(g_error).c_str());
        } else {
            ImGui::TextUnformatted("选择一个卷，点击“扫描”。");
            if (!g_volumeAccessible)
                ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1),
                                   "未以管理员身份运行：MFT 直读需要管理员权限。");
        }
        ImGui::EndChild();
        return;
    }

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##filter", "筛选名称…", g_filter, sizeof(g_filter)))
        g_orderDirty = true;
    if (g_orderDirty) refresh_children();

    const uint64_t total = node_size(g_current);

    const ImGuiTableFlags tflags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##children", 3, tflags, ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("大小", ImGuiTableColumnFlags_WidthFixed, 104.0f);
        ImGui::TableSetupColumn("占比", ImGuiTableColumnFlags_WidthFixed, 76.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        header_cell("名称", 0);
        header_cell("大小", 1);
        header_cell("占比", 2);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float cellPad = ImGui::GetStyle().CellPadding.y;
        for (uint32_t c : g_childOrder) draw_child_row(c, total, dl, cellPad);
        if (g_childOrder.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled(g_filter[0] != '\0' ? "无匹配项" : "没有可显示的项");
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void draw_status() {
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::BeginChild("##status", ImVec2(0, ImGui::GetFrameHeight() + st.WindowPadding.y * 2.0f),
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    if (g_scanning.load()) {
        ImGui::TextUnformatted("正在扫描...");
    } else if (!g_error.empty()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", cs::wide_to_utf8(g_error).c_str());
    } else if (g_current != cs::VolumeTree::kInvalid) {
        const uint64_t total = node_size(g_tree.root());
        ImGui::Text("合计 %s", cs::human_size(total).c_str());
        ImGui::SameLine(0, 16);
        ImGui::TextDisabled("文件 %llu",
                            static_cast<unsigned long long>(g_progress.fileCount.load()));
        ImGui::SameLine(0, 12);
        ImGui::TextDisabled("文件夹 %llu",
                            static_cast<unsigned long long>(g_progress.dirCount.load()));
        if (g_filteredCount > 0) {
            ImGui::SameLine(0, 16);
            ImGui::TextDisabled("已过滤 %llu 项 / 合计 %s",
                                static_cast<unsigned long long>(g_filteredCount),
                                cs::human_size(g_filteredBytes).c_str());
        }
        const uint64_t orphN = g_progress.orphanNodes.load();
        const uint64_t orphB = g_progress.orphanBytes.load();
        if (orphN > 0 && orphB * 500u > total) {  // only when non-negligible (>0.2%)
            ImGui::SameLine(0, 16);
            ImGui::TextDisabled("未连接 %llu",
                                static_cast<unsigned long long>(orphN));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("未能归入目录树的记录（约 %s）：MFT 解析的残留项",
                                  cs::human_size(orphB).c_str());
        }
    } else {
        ImGui::TextDisabled("%zu 个 NTFS 卷，选择后点击“扫描”", g_volumes.size());
    }

    // Right-aligned legend.
    float legendW = ImGui::CalcTextSize("图例").x + 10.0f;
    for (uint8_t cat = 1; cat <= 7; ++cat)
        legendW += 14.0f + ImGui::CalcTextSize(category_name(cat)).x + 8.0f;
    const float x = ImGui::GetWindowContentRegionMax().x - legendW;
    if (x > ImGui::GetCursorPosX() + 20.0f) {
        ImGui::SameLine(x);
        ImGui::TextDisabled("图例");
        for (uint8_t cat = 1; cat <= 7; ++cat) {
            ImGui::SameLine();
            const ImU32 col = category_color(cat);
            const ImVec2 sp = ImGui::GetCursorScreenPos();
            const float ty = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(sp.x, sp.y + (ty - 9.0f) * 0.5f),
                ImVec2(sp.x + 10.0f, sp.y + (ty + 9.0f) * 0.5f), col, 2.0f);
            ImGui::Dummy(ImVec2(13.0f, 0.0f));
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(category_name(cat));
        }
    }
    ImGui::EndChild();
}

void draw_frame() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

    draw_toolbar();

    const ImGuiStyle& st = ImGui::GetStyle();
    const float statusH = ImGui::GetFrameHeight() + st.WindowPadding.y * 2.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float panelH = avail.y - statusH - st.ItemSpacing.y;

    draw_list_panel(0.0f, panelH);
    draw_status();

    ImGui::End();
}

}  // namespace

int run() {
    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SpaceLensWnd";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"SpaceLens - 磁盘空间分析",
                              WS_OVERLAPPEDWINDOW, 80, 80, 1280, 820, nullptr, nullptr,
                              wc.hInstance, nullptr);
    if (hwnd == nullptr || !create_device(hwnd)) {
        cleanup_device();
        if (hwnd) DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    if (!create_rtv()) {
        dbg("create_rtv failed");
        cleanup_device();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    dbg("device + swapchain created");
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    setup_style(scale);
    load_font(scale);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    dbg("imgui dx11 initialized");

    g_volumes = cs::list_ntfs_volumes();
    g_volumeAccessible = can_open_volume();

    std::wstring devDir;
    if (dev_recursive_dir(devDir)) start_scan();  // auto-start for dev smoke tests
    {
        wchar_t cap[1024] = {};
        if (GetEnvironmentVariableW(L"SPACELENS_CAPTURE", cap, 1024) > 0)
            g_capturePath = cap;
    }

    unsigned long long frame = 0;
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_swapOccluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        g_swapOccluded = false;

        if (g_resizeW != 0 && g_resizeH != 0) {
            cleanup_rtv();
            const HRESULT resizeHr =
                g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = 0;
            g_resizeH = 0;
            // 中文注释：ResizeBuffers 失败时保留空 RTV，下面会跳过渲染而不是
            // 把空指针交给 OMSetRenderTargets/ClearRenderTargetView。
            if (FAILED(resizeHr) || !create_rtv()) dbg("resize/recreate RTV failed");
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        poll_scan();
        draw_frame();

        ImGui::Render();
        const float clear[4] = {0.07f, 0.07f, 0.08f, 1.0f};
        if (g_rtv != nullptr) {
            g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
            g_context->ClearRenderTargetView(g_rtv, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }

        if (!g_capturePath.empty() && frame == 120) {
            save_backbuffer_bmp(g_capturePath);
            dbg("captured back buffer");
            done = true;
        }

        const HRESULT hr = g_swapChain->Present(1, 0);  // vsync
        g_swapOccluded = (hr == DXGI_STATUS_OCCLUDED);
        ++frame;
        if (frame == 1) dbg("frame 1 rendered");
        else if (frame % 120 == 0) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "frame %llu, nodes=%zu",
                          static_cast<unsigned long long>(frame), g_tree.nodeCount());
            dbg(buf);
        }
    }

    // 中文注释：退出前先请求取消并取回 future，避免后台线程继续访问即将销毁的
    // 全局对象（也更不会在进程退出阶段触发 std::terminate）。
    g_progress.cancel.store(true, std::memory_order_relaxed);
    if (g_scanFuture.valid()) {
        try {
            g_scanFuture.get();
        } catch (...) {
        }
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

}  // namespace gui
