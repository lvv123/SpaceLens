#include "app.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "format.hpp"
#include "scan.hpp"
#include "tree.hpp"
#include "treemap_view.hpp"
#include "utf8.hpp"

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace gui {
namespace {

// Optional debug log: set COMPUTE_SPACE_LOG to a file path.
void dbg(const char* msg) {
    static FILE* f = nullptr;
    static bool initialized = false;
    if (!initialized) {
        initialized = true;
        wchar_t path[1024] = {};
        if (GetEnvironmentVariableW(L"COMPUTE_SPACE_LOG", path, 1024) > 0) {
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

void create_rtv() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) && back != nullptr) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
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

// --------------------------------------------------------------------- State
cs::VolumeTree            g_tree;
std::vector<std::wstring> g_volumes;
int                       g_volumeIndex = 0;
std::thread               g_worker;
cs::ScanProgress          g_progress;
std::atomic<bool>         g_scanning{false};
std::vector<uint32_t>     g_path;
uint32_t                  g_current = cs::VolumeTree::kInvalid;
uint64_t                  g_threshold = 1u << 20;
gui::SizeBasis            g_basis = gui::SizeBasis::Logical;
std::wstring              g_error;
std::vector<uint32_t>     g_otherNodes;
bool                      g_openOtherPopup = false;
bool                      g_volumeAccessible = true;

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
    g_progress.recordsRead.store(0);
    g_progress.totalRecords.store(0);
    g_progress.bytesRead.store(0);
    g_progress.cancel.store(false);
    g_progress.finished.store(false);
    g_progress.failed.store(false);
}

// Dev-only: when COMPUTE_SPACE_DEV_DIR is set, scan that directory with the
// portable recursive scanner instead of MFT. Lets the GUI be exercised without
// administrator rights. Never set in normal runs.
bool dev_recursive_dir(std::wstring& out) {
    wchar_t buf[1024] = {};
    const DWORD n = GetEnvironmentVariableW(L"COMPUTE_SPACE_DEV_DIR", buf, 1024);
    if (n == 0 || n >= 1024) return false;
    out.assign(buf, n);
    return true;
}

void start_scan() {
    if (g_worker.joinable()) g_worker.join();
    g_tree.clear();
    g_path.clear();
    g_current = cs::VolumeTree::kInvalid;
    g_error.clear();
    g_otherNodes.clear();
    reset_progress();
    g_scanning.store(true);

    std::wstring devDir;
    if (dev_recursive_dir(devDir)) {
        g_worker = std::thread([devDir]() {
            cs::VolumeTree local;
            std::wstring err;
            if (cs::scan_directory_recursive(devDir, local, g_progress, err)) {
                g_tree = std::move(local);
            } else {
                g_error = err;
            }
        });
        return;
    }

    const std::wstring vol = g_volumes.empty() ? std::wstring(L"C:") : g_volumes[g_volumeIndex];
    g_worker = std::thread([vol]() {
        cs::VolumeTree local;
        std::wstring err;
        const bool ok = cs::scan_volume_mft(vol, local, g_progress, err);
        if (ok) {
            g_tree = std::move(local);
        } else {
            g_error = err;
        }
    });
}

void poll_scan() {
    if (!g_scanning.load()) return;
    if (g_progress.finished.load() || g_progress.failed.load()) {
        if (g_worker.joinable()) g_worker.join();
        g_scanning.store(false);
        if (!g_progress.failed.load() && g_tree.nodeCount() > 0) {
            g_current = g_tree.root();
            g_path.assign(1, g_current);
            const uint64_t rs = g_tree.node(g_current).totalSize;
            g_threshold = (rs > 1000) ? rs / 1000 : 1024;
        }
    }
}

void navigate_to(uint32_t node) {
    if (!g_tree.valid(node)) return;
    g_current = node;
    g_path.push_back(node);
}

void navigate_up() {
    if (g_path.size() > 1) {
        g_path.pop_back();
        g_current = g_path.back();
    }
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
    ImGui::TextUnformatted("卷:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    const std::string currentVol =
        g_volumes.empty() ? std::string("(无)") : cs::wide_to_utf8(g_volumes[g_volumeIndex]);
    if (ImGui::BeginCombo("##vol", currentVol.c_str())) {
        for (int i = 0; i < static_cast<int>(g_volumes.size()); ++i) {
            const std::string label = cs::wide_to_utf8(g_volumes[i]);
            if (ImGui::Selectable(label.c_str(), i == g_volumeIndex)) g_volumeIndex = i;
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(g_scanning.load());
    if (ImGui::Button("扫描")) start_scan();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_scanning.load());
    if (ImGui::Button("取消")) g_progress.cancel.store(true);
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextUnformatted("阈值:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(190);
    float logv = (g_threshold > 0) ? static_cast<float>(std::log10(static_cast<double>(g_threshold)))
                                   : 3.0f;
    if (logv < 3.0f) logv = 3.0f;
    if (ImGui::SliderFloat("##thresh", &logv, 3.0f, 10.5f, "")) {
        g_threshold = static_cast<uint64_t>(std::pow(10.0, static_cast<double>(logv)));
        if (g_threshold < 1024) g_threshold = 1024;
    }
    ImGui::SameLine();
    ImGui::Text("%s", cs::human_size(g_threshold).c_str());

    ImGui::SameLine();
    ImGui::TextUnformatted("面积:");
    ImGui::SameLine();
    int basis = static_cast<int>(g_basis);
    if (ImGui::RadioButton("逻辑", basis == 0)) basis = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("占用", basis == 1)) basis = 1;
    g_basis = static_cast<gui::SizeBasis>(basis);
}

void draw_other_popup() {
    if (g_openOtherPopup) {
        ImGui::OpenPopup("已过滤的项");
        g_openOtherPopup = false;
    }
    if (ImGui::BeginPopupModal("已过滤的项", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("小于阈值 %s 的项（共 %zu 个）：", cs::human_size(g_threshold).c_str(),
                    g_otherNodes.size());
        ImGui::BeginChild("##otherlist", ImVec2(460, 320));
        int shown = 0;
        for (uint32_t n : g_otherNodes) {
            if (shown++ >= 500) {
                ImGui::TextUnformatted("... (列表已截断)");
                break;
            }
            const uint64_t sz =
                (g_basis == gui::SizeBasis::Logical) ? g_tree.node(n).totalSize
                                                     : g_tree.node(n).totalAlloc;
            ImGui::Text("%s\t%s", cs::wide_to_utf8(g_tree.nameString(n)).c_str(),
                        cs::human_size(sz).c_str());
        }
        ImGui::EndChild();
        if (ImGui::Button("关闭")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void draw_treemap_panel(float width, float height) {
    ImGui::BeginChild("treemap", ImVec2(width, height), true, ImGuiWindowFlags_NoScrollbar);
    if (g_scanning.load()) {
        const uint64_t total = g_progress.totalRecords.load();
        const uint64_t read = g_progress.recordsRead.load();
        const float frac = (total > 0) ? static_cast<float>(static_cast<double>(read) / total) : 0.0f;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%llu / %llu", static_cast<unsigned long long>(read),
                      static_cast<unsigned long long>(total));
        ImGui::TextUnformatted("正在扫描 MFT...");
        ImGui::ProgressBar(frac, ImVec2(-1, 0), buf);
        ImGui::Text("已读取 %s", cs::human_size(g_progress.bytesRead.load()).c_str());
    } else if (g_current != cs::VolumeTree::kInvalid) {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 size = ImGui::GetContentRegionAvail();
        cs::Rect bounds{pos.x, pos.y, size.x, size.y};
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const TreemapResult res = gui::draw_treemap(dl, g_tree, g_current, bounds, g_threshold,
                                                    g_basis, ImGui::IsWindowHovered());
        if (res.otherClicked) {
            g_otherNodes = res.otherNodes;
            g_openOtherPopup = true;
        } else if (res.clickedTree != cs::VolumeTree::kInvalid && g_tree.isDirectory(res.clickedTree) &&
                   g_tree.node(res.clickedTree).childCount > 0) {
            navigate_to(res.clickedTree);
        }
    } else if (!g_error.empty()) {
        ImGui::TextWrapped("扫描失败：%s", cs::wide_to_utf8(g_error).c_str());
    } else {
        ImGui::TextUnformatted("选择一个卷，点击“扫描”。");
        if (!g_volumeAccessible)
            ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1),
                               "未以管理员身份运行：MFT 直读需要管理员权限。");
    }
    ImGui::EndChild();
}

void draw_list_panel(float width, float height) {
    ImGui::BeginChild("list", ImVec2(width, height), true);
    if (g_path.size() > 1) {
        if (ImGui::Button("< 上级")) navigate_up();
        ImGui::SameLine();
    }
    std::string crumb;
    for (size_t i = 0; i < g_path.size(); ++i) {
        crumb += cs::wide_to_utf8(g_tree.nameString(g_path[i]));
        if (i + 1 < g_path.size()) crumb += " / ";
    }
    ImGui::TextWrapped("%s", crumb.c_str());
    ImGui::Separator();

    if (g_current != cs::VolumeTree::kInvalid) {
        const auto& cur = g_tree.node(g_current);
        const uint64_t total = cur.totalSize;
        ImGui::BeginChild("##children");
        for (uint32_t c = cur.firstChild; c != cs::VolumeTree::kInvalid;
             c = g_tree.node(c).nextSibling) {
            const auto& n = g_tree.node(c);
            const double pct = total > 0 ? 100.0 * static_cast<double>(n.totalSize) /
                                               static_cast<double>(total)
                                         : 0.0;
            char label[512];
            std::snprintf(label, sizeof(label), "%s%s    %s    %.1f%%",
                          g_tree.isDirectory(c) ? "[D] " : "",
                          cs::wide_to_utf8(g_tree.nameString(c)).c_str(),
                          cs::human_size(n.totalSize).c_str(), pct);
            if (ImGui::Selectable(label)) {
                if (g_tree.isDirectory(c) && n.childCount > 0) navigate_to(c);
            }
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
}

void draw_status() {
    ImGui::Separator();
    if (g_scanning.load()) {
        ImGui::TextUnformatted("扫描中...");
    } else if (!g_error.empty()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", cs::wide_to_utf8(g_error).c_str());
    } else if (g_current != cs::VolumeTree::kInvalid) {
        const uint64_t total = g_tree.node(g_tree.root()).totalSize;
        ImGui::Text("合计 %s | 节点 %zu | 当前 %s", cs::human_size(total).c_str(),
                    g_tree.nodeCount(),
                    cs::wide_to_utf8(g_tree.nameString(g_current)).c_str());
    } else {
        ImGui::Text("%zu 个 NTFS 卷", g_volumes.size());
    }

    ImGui::SameLine(0.0f, 24.0f);
    ImGui::TextUnformatted("图例:");
    for (uint8_t cat = 1; cat <= 7; ++cat) {
        ImGui::SameLine();
        const ImU32 col = gui::category_color(cat);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col), "%s", gui::category_name(cat));
    }
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
    ImGui::Separator();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float statusH = ImGui::GetFrameHeightWithSpacing() + 6.0f;
    const float panelH = avail.y - statusH;
    const float leftW = avail.x * 0.60f;

    draw_treemap_panel(leftW, panelH);
    ImGui::SameLine();
    draw_list_panel(0.0f, panelH);
    draw_status();
    draw_other_popup();

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
    wc.lpszClassName = L"ComputeSpaceV2Wnd";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Compute Space v2 - 磁盘空间分析",
                              WS_OVERLAPPEDWINDOW, 80, 80, 1280, 820, nullptr, nullptr,
                              wc.hInstance, nullptr);
    if (hwnd == nullptr || !create_device(hwnd)) {
        cleanup_device();
        if (hwnd) DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    create_rtv();
    dbg("device + swapchain created");
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    ImGui::GetStyle().ScaleAllSizes(scale);
    load_font(scale);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    dbg("imgui dx11 initialized");

    g_volumes = cs::list_ntfs_volumes();
    g_volumeAccessible = can_open_volume();

    std::wstring devDir;
    if (dev_recursive_dir(devDir)) start_scan();  // auto-start for dev smoke tests

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
            g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = 0;
            g_resizeH = 0;
            create_rtv();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        poll_scan();
        draw_frame();

        ImGui::Render();
        const float clear[4] = {0.07f, 0.07f, 0.08f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

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

    if (g_worker.joinable()) g_worker.join();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

}  // namespace gui
