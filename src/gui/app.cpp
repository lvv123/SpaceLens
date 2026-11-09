// =============================================================================
// SpaceLens 的 Win32 + DirectX 11 + Dear ImGui 前端。
//
// 这个文件只做三件事：
//   1. 把窗口、swap chain、RTV 与 ImGui 后端这条渲染链路搭起来并维持存活；
//   2. 把核心库（MFT 扫描 -> VolumeTree）的结果用“按大小排序的逐层列表”呈现；
//   3. 管理扫描线程的生命周期：工作线程算，UI 线程收割并绘制。
//
// 线程模型（全文件最重要的约定）：
//   - 扫描线程只写 ScanOutcome（自己的局部数据）与 g_progress（全 atomic 字段）；
//   - 目录树等重数据通过 std::future 交回，poll_scan() 在 UI 线程一次性安装；
//   - 因此任何绘制代码都可以认定 g_tree 没有其他线程在访问。
//
// 每帧的固定顺序：消息泵 -> 遮挡/尺寸检查 -> ImGui NewFrame ->
// poll_scan -> draw_frame -> Render -> Present。这个顺序不能随意调整。
//
// 开发用环境变量（正常使用都不需要设置）：
//   SPACELENS_LOG     追加写调试日志（GUI 无控制台，这是唯一观测手段）；
//   SPACELENS_CAPTURE 第 120 帧导出客户区 BMP 后退出，用于无头视觉检查；
//   SPACELENS_DEV_DIR 改为递归扫描指定目录，免管理员权限跑通整条 UI。
// =============================================================================
#include "app.hpp"

// 平台头：windows.h 必须最先引入（d3d11.h/dxgi.h 依赖其类型），
// shellapi.h 则只为右键菜单里的 ShellExecuteW 提供声明。
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>

// 标准库：algorithm 用于子项排序，atomic/future 支撑扫描线程，
// cmath 提供阈值滑杆的 log10/pow，cstdio 与 cstring 服务于调试日志与 BMP 头。
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

// ImGui 本体与两个后端：DX11 后端负责 GPU 资源与提交，Win32 后端负责
// 输入、光标与 DPI 查询。二者必须与主版本严格配套（IMGUI_CHECKVERSION 会校验）。
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

// 核心库：format 提供 human_size，scan 提供两条扫描入口与进度结构，
// tree 是 arena 目录树，utf8 负责宽字符与 UTF-8 互转（界面字符串统一 UTF-8）。
#include "format.hpp"
#include "scan.hpp"
#include "tree.hpp"
#include "utf8.hpp"

// 引入顺序有意保持四组：本文件头 -> 平台头 -> 标准库 -> 第三方与核心库。
// 其中 ImGui 后端头必须先看到 Win32 与 DX11 的类型，所以只能排在平台头之后。
//
// ImGui 的 Win32 消息回调默认只在它自己的 .cpp 里可见，也没有对外导出头文件；
// 这里手动声明，才能在 wnd_proc 里把消息先交给 ImGui 处理（鼠标/键盘/IME）。
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace gui {
namespace {
// 匿名命名空间让下面所有类型和函数都只在本翻译单元可见，
// 既避免了链接期符号冲突，也允许编译器把纯局部函数内联掉。

// ------------------------------------------------------------ 分类与调试辅助
// 大小口径：逻辑大小 = 文件真实字节数，占用大小 = 按簇向上取整后的磁盘分配量。
// 目录的两个值都是子树累加结果（见 VolumeTree::aggregate）。
// 底层类型写成 int，是为了能直接和 ImGui 的 RadioButton 下标比较（见 draw_toolbar）。
enum class SizeBasis : int { Logical = 0, OnDisk = 1 };

// 分类色板：色相刻意分散（红/橙/紫/蓝/橄榄/绿/青），保证在深色背景的占比条
// 与图例小方块上互相可分辨；统一 alpha 235 让色块压在行底色上仍略透。
// `category` 是 VolumeTree::Node 里存的原始字节，所以先转成枚举再分支；
// default 同时兜住 Category::Other(0) 和未来新增但此处未识别的取值。
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

// 图例与提示里显示的分类名。分支顺序必须与 tree.hpp 的 Category 枚举一致；
// default 覆盖 Other(0)，因此 draw_status 的图例从 1 遍历到 7 不会重复出现“其他”。
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

// GUI 子系统没有控制台，stdout 不可见，所以调试信息只能落到环境变量指定的
// 文件：SPACELENS_LOG=<路径>，以追加方式打开，多次运行不会互相覆盖。
// 文件句柄与初始化标志都是函数内 static：进程生命周期内只探测一次环境变量，
// 之后每次调用直接写。每条都 fflush，因为调试场景经常是“写两行后崩溃”，
// 缓冲留在用户态就全丢了。调用方都来自 UI 线程，故不额外加锁。
// 日志不做轮转：调试时请自行清理，避免长时间运行把磁盘写满。
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
// 这组对象的生命周期贯穿整个进程：create_device() 建，cleanup_device() 拆。
// 存在依赖关系——RTV 引用着 swap chain 的后台缓冲，所以销毁时必须先 RTV
// 后 swap chain（cleanup_device 内部已按此顺序调用 cleanup_rtv），
// 顺序反了会在调试层报“对象仍被引用”。
ID3D11Device*            g_device = nullptr;
ID3D11DeviceContext*     g_context = nullptr;
IDXGISwapChain*          g_swapChain = nullptr;
ID3D11RenderTargetView*  g_rtv = nullptr;
// g_swapOccluded 记录上一次 Present 是否返回 DXGI_STATUS_OCCLUDED：
// 窗口被完全遮挡（锁屏、RDP 断开、最小化）时先做一次 PRESENT_TEST 试探，
// 仍被遮挡就 Sleep 让出 CPU，避免无人看画面时空转。
bool                     g_swapOccluded = false;
// WM_SIZE 只把尺寸记到这两个变量里，真正的 ResizeBuffers 由渲染循环执行；
// 因此它们表示“待处理的尺寸请求”，处理完立即清零（0 表示没有待处理请求）。
UINT                     g_resizeW = 0;
UINT                     g_resizeH = 0;

// 创建 D3D11 设备与 swap chain（一次调用同时完成两件事，省掉手动
// CreateSwapChain 的 DXGI 工厂配置）。
// - BufferCount=2 + DISCARD：最保守的组合，兼容老系统与 WARP，不使用 flip model；
// - R8G8B8A8_UNORM：ImGui 后端按此格式写顶点色/纹理，BMP 抓图也按 4 字节/像素处理；
// - RefreshRate 在窗口模式下被忽略，只是给全屏 Alt+Enter 一个初值；
// - ALLOW_MODE_SWITCH 允许 Alt+Enter 切换独占全屏，调试时用得上；
// - SampleDesc.Count=1：不做 MSAA，否则 CopyResource 到 staging 抓图会失败；
// - 先试 11_0，再退到 10_0；某些虚拟机/远程桌面只有 WARP 软光栅，
//   DXGI_ERROR_UNSUPPORTED 会触发第二次尝试，保证程序仍能出画面（很慢）。
// 返回 false 时调用方直接退出，不去猜是哪一步失败。
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

    // 只有这两个特性级别在目标机器上都失败才算彻底不支持；
    // level 是输出参数，当前实现不需要它，但仍然必须传入合法地址。
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

// 前置声明：create_rtv 在重建前要先释放旧视图，而 cleanup_rtv 定义在其后。
void cleanup_rtv();  // 定义在 create_rtv 之后

// 从 swap chain 的后台缓冲创建 RTV。任何一步失败都返回 false 并把 g_rtv 置空，
// 调用方据此跳过本帧渲染。绝不能返回 true 却留下空指针，否则
// OMSetRenderTargets/ClearRenderTargetView 会收到 null，调试层直接报错。
// GetBuffer 会 AddRef 后台缓冲，RTV 建好后即可 Release，视图自身持有引用。
// 每次 ResizeBuffers 之后后台缓冲都会被替换，所以 RTV 必须跟着重建。
bool create_rtv() {
    // 创建失败必须返回 false；否则后续会用空 RTV 清屏，DX11 会报错。
    cleanup_rtv();
    ID3D11Texture2D* back = nullptr;
    const HRESULT getHr = g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (FAILED(getHr) || back == nullptr) return false;
    // CreateRenderTargetView 的第二个参数传 nullptr：表示用整个资源做视图，
    // 这里没有 mip/数组切片需求，省掉手工填 D3D11_RENDER_TARGET_VIEW_DESC。
    const HRESULT rtvHr = g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
    if (FAILED(rtvHr)) {
        g_rtv = nullptr;
        return false;
    }
    return g_rtv != nullptr;
}

// 幂等释放：失败路径与重建路径都可能重复调用，Release 前判空避免解引用空指针。
void cleanup_rtv() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

// 退出顺序：RTV -> swap chain -> context -> device。
// 隐式交换链可能在最后一个引用释放时才真正销毁，因此不要先放 device。
void cleanup_device() {
    cleanup_rtv();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context)   { g_context->Release();   g_context = nullptr; }
    if (g_device)    { g_device->Release();    g_device = nullptr; }
}

// 开发/QA 用：把当前后台缓冲导出成 32 位 BMP（SPACELENS_CAPTURE=<文件.bmp>）。
// 后台缓冲不能直接 Map，必须先用同尺寸/同格式的 STAGING 纹理 + CopyResource
// 拷一份 CPU 可读的副本；Map 出来的 RowPitch 可能大于 w*4（行对齐），
// 所以逐行按 RowPitch 取源、按紧凑的 row_bytes 写出。
// BMP 头固定 54 字节；高度取负值表示自上而下的行序，与 DX 的行序一致，
// 省掉整幅翻转。32bpp 行天然 4 字节对齐，不需要补 padding。
// DXGI 是 RGBA，BMP 期望 BGRA，所以逐像素交换 R/B；alpha 一律写 255，
// 因为 BMP 的 alpha 通道在多数查看器里不被解释。
// 上述 CopyResource 只在没有 MSAA 时成立（create_device 里 Count=1 保证了这点）。
void save_backbuffer_bmp(const std::wstring& path) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) || back == nullptr) return;
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    // 复制描述后只改 CPU 访问相关字段：尺寸/格式/MipLevels 必须与源完全一致，
    // 否则 CopyResource 会静默失败（调试层会报参数不匹配）。
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
                // 逐字段手工填 BITMAPFILEHEADER + BITMAPINFOHEADER，
                // 免去引入 <wingdi.h> 的 CreateDIBSection 体系（这里只需要写文件）。
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
                    for (uint32_t x = 0; x < w; ++x) {  // RGBA 转 BGRA
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
// 线程模型：扫描跑在 std::async 的独立线程里，它只写自己的 ScanOutcome 和
// 共享的 g_progress（字段都是 atomic，供进度条只读轮询）。目录树、错误文本
// 等“重”数据一律通过 future 交回 UI 线程，由 poll_scan() 一次性安装，
// 因此绘制期间不会再有人同时遍历 g_tree。
struct ScanOutcome {
    bool ok = false;
    cs::VolumeTree tree;
    std::wstring error;
};

// 当前显示的树。只有 UI 线程写；poll_scan() 用 move 赋值把工作线程构造好的
// arena 整体接管过来，避免复制几十万个节点。同时它也保证了“没有并发访问”：
// 工作线程交出来的 out.tree 在 get() 之后已经归 UI 线程所有。
cs::VolumeTree            g_tree;
// NTFS 卷列表（形如 "C:"、"D:"），启动时探测一次；g_volumeIndex 是下拉框选中项。
// 注意扫描线程用的是点击那一刻捕获的卷名，之后改下拉框不会影响已开始的任务。
std::vector<std::wstring> g_volumes;
int                       g_volumeIndex = 0;
// 正在运行的扫描任务句柄。valid() 为 false 表示没有任务；重新扫描或退出前
// 必须先 get() 取回结果，因为 std::future 析构会等待 launch::async 的任务结束，
// 若带着任务析构就会在 UI 线程上阻塞。
std::future<ScanOutcome>  g_scanFuture;
// 进度/取消/完成标志由工作线程写、UI 线程读，全部是 atomic；这里始终是同一个对象，
// 工作线程按引用捕获它，所以它的寿命必须覆盖整个进程（这也是它不做局部变量的原因）。
// 几组字段的用途值得单独说明：
//   recordsRead/totalRecords  驱动扫描进度条（recordsRead 可能短暂超过 total，UI 只做近似）；
//   bytesRead                 已读卷字节数，不是“已统计文件的大小”；
//   fileCount/dirCount        本轮扫描的全局累计，状态栏直接显示，下钻时不变；
//   orphan*                   建了节点却挂不到父目录的记录，序列号不匹配是常见原因；
//   cancel/finished/failed    三方握手：UI 置 cancel，工作线程置 finished/failed。
cs::ScanProgress          g_progress;
// 扫描是否进行中。用 atomic 而不是普通 bool：主循环、工具栏、状态栏都读它，
// 而写它的是 poll_scan/start_scan；将来即便挪到别的线程也仍然成立。
std::atomic<bool>         g_scanning{false};
// 面包屑路径：从根到当前节点的一串节点索引，满足 g_path.back() == g_current。
// 不靠 parent 链现算，是为了让“上一级”和面包屑点击都是 O(1) 下标操作，
// 同时也天然记录了用户的下钻历史（只允许通过面包屑回退，不维护前进历史）。
std::vector<uint32_t>     g_path;
// 当前目录节点；kInvalid 表示还没有扫描结果（此时列表区显示占位提示）。
uint32_t                  g_current = cs::VolumeTree::kInvalid;
// 展示阈值与当前大小口径：只影响列表过滤与排序，不改变任何统计值。
// 默认 1MiB，只是为了在尚未扫描时滑杆有个合理刻度；扫描成功后会按根总量重算。
uint64_t                  g_threshold = 1u << 20;
SizeBasis                 g_basis = SizeBasis::Logical;
// 最近一次扫描的错误文本（宽字符，直接来自核心库），非空时状态栏红字显示。
std::wstring              g_error;
// 被阈值隐藏的项数与字节数，供状态栏的“已过滤”提示；每次 refresh_children 重算。
// 注意名字筛选项不计入这两个值，否则用户会误以为“过滤”包含搜索。
uint64_t                  g_filteredCount = 0;
uint64_t                  g_filteredBytes = 0;
// 启动时探测的卷句柄可打开性，仅用于在 UI 上提示“需要管理员权限”。
bool                      g_volumeAccessible = true;
// 抓图输出路径（环境变量 SPACELENS_CAPTURE）；非空时在第 120 帧导出 BMP 后退出。
std::wstring              g_capturePath;  // 只由环境变量设置，正常运行为空

// 当前目录的可见子项索引（已按筛选/阈值过滤并排序）。绘制时只遍历它，
// 不再碰兄弟链，因此排序结果与表格行序是同一份数据，不会错位。
// 存索引而不是指针/引用：poll_scan 会用 move 整体替换 g_tree，
// 届时任何指针都会失效，而索引在新树上依然指向同一逻辑节点。
std::vector<uint32_t> g_childOrder;      // 只在当帧绘制时遍历
// 排序状态：列 0=名称、1=大小、2=占比；方向对整个表生效而不是逐列记忆。
// 默认按大小降序，符合“先看谁最占空间”的使用目的。
int                   g_sortCol = 1;     // 1 = 大小列，初始排序键
bool                  g_sortAscending = false;
// 脏标记：阈值/口径/筛选/面包屑/排序任意变化都只置它，
// 真正的 rebuild 集中到 draw_list_panel 里的一个点，保证每帧最多一次。
bool                  g_orderDirty = true;
// 名称筛选缓冲，InputTextWithHint 直接写这块固定内存（UTF-8 字节数，非字符数）。
// 96 字节足够容纳常见关键词，超长部分由 ImGui 自动截断。
char                  g_filter[96] = "";

// 窗口过程：先交给 ImGui 的 Win32 后端，它负责鼠标捕获、滚轮、键盘与 IME；
// 返回 true 表示消息已被消费，不再往下传。
// WM_SIZE 只记录尺寸就返回：ResizeBuffers 不能在窗口过程里做（此时 swap chain
// 可能还在 Present 中），渲染循环会在下一帧统一处理。最小化时宽高为 0，
// 直接忽略可以避免拿 0x0 去 Resize。
// 连续拖动窗口会产生多串 WM_SIZE，这里只保留最后一个尺寸，
// ResizeBuffers 每帧最多执行一次，天然完成了合并。
// WM_SYSCOMMAND 过滤 SC_KEYMENU：屏蔽 Alt 单独按下弹出的系统菜单，
// 否则 ImGui 里的 Alt 组合键会先被系统菜单抢走。
// WM_DESTROY 投递退出消息，主循环看到 WM_QUIT 后走正常清理流程。
LRESULT WINAPI wnd_proc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            g_resizeW = static_cast<UINT>(LOWORD(lParam));
            g_resizeH = static_cast<UINT>(HIWORD(lParam));
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // 屏蔽 Alt 系统菜单
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// 探测当前进程能否打开卷句柄（MFT 直读的前提）。这里只探测 C:：在正常权限下
// 所有本地 NTFS 卷的打开权限是一致的，因此一个探针就够；打开后立即关闭，
// 仅用于在 UI 上给出“需要管理员权限”的提示，不影响真正的扫描（扫描会自己再开一次）。
// 共享模式必须允许读写共享，否则卷上只要还有活动句柄就会打开失败。
// 打开后立刻 CloseHandle，不长期持有卷句柄：占用卷可能让其他程序
// 无法执行需要独占的操作（例如 chkdsk）。
bool can_open_volume() {
    HANDLE h = CreateFileW(L"\\\\.\\C:", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// 统一走 ScanProgress::reset()：将来新增计数字段时只需改那一处，
// 不会出现“这里漏清一个字段、下一轮进度条从旧值开始”的问题。
void reset_progress() {
    g_progress.reset();
}

// 开发专用：设置 SPACELENS_DEV_DIR 时改用可移植的递归扫描器扫该目录，
// 这样不需要管理员权限也能跑通整条 UI 链路（冒烟测试、无头截图）。
// 返回的 n >= 1024 说明环境变量被缓冲区截断，按“未设置”处理更安全。
// 正常运行时绝对不要设置这个变量。
bool dev_recursive_dir(std::wstring& out) {
    wchar_t buf[1024] = {};
    const DWORD n = GetEnvironmentVariableW(L"SPACELENS_DEV_DIR", buf, 1024);
    if (n == 0 || n >= 1024) return false;
    out.assign(buf, n);
    return true;
}

// 开始一次扫描。入口处先对上一次任务做防御性收尾：设置 cancel 再 get()，
// 保证 future 处于 invalid 状态才能被 async 重新赋值——对 valid 的 future
// 赋值会阻塞到旧任务结束，容易在 UI 线程上造成假死。
// get() 会重新抛出工作线程的异常，这里全部吞掉：错误已经通过 ScanOutcome 表达。
void start_scan() {
    // 正常流程下扫描按钮在扫描期间是禁用的；这里仍防御性收尾上一次任务，
    // 保证 std::future 在复用前已经取回结果。
    if (g_scanFuture.valid()) {
        // 这里先置 cancel 再 get() 是关键顺序：反过来的话，旧任务可能还要跑很久，
        // 期间 UI 线程被 get() 卡住，窗口会短暂显示“未响应”。
        g_progress.cancel.store(true, std::memory_order_relaxed);
        try {
            g_scanFuture.get();
        } catch (...) {
        }
    }
    // 清空旧树/路径/错误，并把进度计数连同 cancel/finished/failed 一起复位。
    // 顺序不能反：reset_progress() 必须在 g_scanning 置 true 之前完成，
    // 否则上一轮遗留的 finished=true 会让 poll_scan 抢在新任务启动前“收割”，
    // 把界面直接判成扫描失败。
    g_tree.clear();
    g_path.clear();
    g_current = cs::VolumeTree::kInvalid;
    g_error.clear();
    // 注意 g_progress 是全局的：本轮任务与上一轮共享同一组计数器，
    // 因此 UI 只能在 finished/failed 置位之后读取（poll_scan 已保证这一点）。
    reset_progress();
    g_scanning.store(true);

    std::wstring devDir;
    if (dev_recursive_dir(devDir)) {
        try {
            // 捕获 devDir 的副本而不是引用：线程启动后栈上的变量可能已离开作用域。
            // 显式 launch::async，避免实现选择 deferred 导致任务根本不跑。
            // 结果只写局部 ScanOutcome；异常只在“起不了线程”时抛出（资源不足），
            // 此时直接回到失败状态，不留下一个永远 running 的 UI。
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

    // 卷名在点击“扫描”这一刻定死；卷列表为空时退到 C:，让 MFT 扫描至少给出
    // 一条具体的错误信息，而不是空字符串导致的“参数无效”。
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

// UI 线程每帧调用：只有工作线程报告 finished/failed 才去收割。
// 顺序很重要——先 get() 拿到完整结果，再安装到全局；中间不做任何绘制，
// 因此 UI 侧看到的一定是“旧树”或“新树”，不会是半成品。
void poll_scan() {
    if (!g_scanning.load()) return;
    if (!g_progress.finished.load() && !g_progress.failed.load()) return;

    // 扫描线程只写自己的局部结果；在这里 get() 之后才把树交给 UI，
    // 因此之后的绘制/日志读取都不会与工作线程并发访问同一棵树。
    // get() 可能抛出：bad_alloc 通常是树太大（亿级节点），其它异常归为未知错误；
    // 两者都转成用户可读的中文，绝不让异常穿过渲染循环。
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
        // 处于 scanning 状态却没有 valid 的 future，说明状态机被破坏；
        // 用一句明确错误收尾，而不是继续显示“正在扫描”。
        out.error = L"扫描任务状态异常";
    }
    g_scanning.store(false);

    if (out.ok && out.tree.nodeCount() > 0) {
        // 安装新树：move 进来后把当前节点指向根、重建面包屑，并标脏让下一帧
        // 统一排序（不在这里重复做一次，避免与列表重建逻辑分叉）。
        g_tree = std::move(out.tree);
        g_current = g_tree.root();
        g_path.assign(1, g_current);
        g_orderDirty = true;
        // 默认阈值取根总量的 0.1%（至少 1KB）：小卷不会因为阈值趋于 0 而
        // 让上万个无关小文件铺满列表，大卷则按同一比例放宽到 GB 级，
        // 打开就是有意义的“大头”，同时不改变任何统计值。
        const uint64_t rs = g_tree.node(g_current).totalSize;
        g_threshold = (rs > 1000) ? rs / 1000 : 1024;
        // 下面这行日志用于诊断解析质量：records 是读过的 MFT 记录数，
        // orphans 是建了节点却没挂到树上的记录及其字节数，再按原因拆成
        // noParent（找不到父记录）与 seqMismatch（父记录序列号不匹配）两栏，
        // 方便区分是卷损坏还是解析器的已知盲区。
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
        // 失败也要把 failed 置位，让状态栏显示错误；核心库没给文案时用兜底措辞。
        // 此时 g_scanning 已在上方置 false，本帧之后不会再进入 poll_scan；
        // failed 要等下一次 start_scan -> reset_progress 才会清除。
        g_error = out.error.empty() ? L"扫描失败" : out.error;
        g_progress.failed.store(true, std::memory_order_relaxed);
    }
}

// 下钻：只改 g_path 与 g_current 并标脏，子项重建推迟到 refresh_children()，
// 因此点击路径上没有任何内存分配，也不用担心在表格遍历途中修改容器。
// 正常调用只来自子项点击，node 必然是当前目录的子项，不会成环；
// valid() 只是防御性检查（例如树被整体替换后遗留的旧索引）。
void navigate_to(uint32_t node) {
    if (!g_tree.valid(node)) return;
    g_current = node;
    g_path.push_back(node);
    g_orderDirty = true;
}

// 上跳一级：路径长度为 1 时已在卷根，保持不动（工具栏按钮在根目录是禁用的）。
void navigate_up() {
    if (g_path.size() > 1) {
        g_path.pop_back();
        g_current = g_path.back();
        g_orderDirty = true;
    }
}

// 只折叠 ASCII：名称筛选是“UTF-8 字节子串”匹配，中文本身没有大小写概念，
// 原样保留即可正确命中；ASCII 部分大小写不敏感更符合用户直觉。
// 先转 unsigned char 再传 tolower：char 为负（UTF-8 多字节的后续字节）
// 时直接把负值传进去是未定义行为。
// 参数按值传递：调用点既会传 g_filter（char*）也会传 wide_to_utf8 的临时量，
// 按值可以在函数内原地折叠并返回，省掉调用方的显式拷贝。
std::string ascii_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// 由节点反推完整文件系统路径。先沿 parent 链收集到根再反向拼接，
// 避免“每层都往字符串头部插入”的 O(depth^2) 行为。
// 根节点的名字本身就是 "C:" 这种形式，所以第一段不加分隔符，
// 后续段之间补一个反斜杠；本项目只列本地 NTFS 盘符，不处理 UNC 路径。
// chain 只在右键菜单“复制路径/在资源管理器中打开”时分配一次，不在每帧的
// 绘制路径上，因此没有为它做缓存或复用缓冲。
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

// 在资源管理器里定位：目标是目录就直接打开它；是文件则用 /select, 让
// 资源管理器打开父目录并选中该文件。路径必须整体加引号，空格和中文才能正确解析。
// 本进程是提权运行的，但 explorer 若已在普通权限下运行，会通过外壳复用现有实例，
// 不会被拉高权限，因此这里不需要额外降权处理。
void open_in_explorer(uint32_t node) {
    const std::wstring path = build_path(node);
    const bool isDir = g_tree.isDirectory(node);
    const std::wstring params = isDir ? (L"\"" + path + L"\"")
                                      : (L"/select,\"" + path + L"\"");
    ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
}

// 把“口径”选择收敛到一个函数：表格排序、占比、阈值过滤都走它，
// 保证同一帧内所有地方算出来的是同一种大小，不会出现“按占用排、按逻辑算占比”。
uint64_t node_size(uint32_t i) {
    return (g_basis == SizeBasis::Logical) ? g_tree.node(i).totalSize
                                           : g_tree.node(i).totalAlloc;
}

// 按当前排序键重建 g_childOrder。
// 关键点：比较器必须构成严格弱序，否则 std::sort 在 Debug 下会触发
// “invalid comparator” 断言，Release 下可能越界写内存（排序实现依赖它推断区间）。
// 因此这里不用“相等就 return !less”的写法（那会让 a<b 与 b<a 同时为真），
// 而是先判断两个方向的小于关系，相等时退回节点索引 a < b —— 索引全局唯一，
// 于是任意两项都有确定次序，排序结果跨帧稳定，相等项不会来回跳动。
// “占比”列复用大小比较：同一父节点下百分比与子树大小严格同序。
// 比较器不捕获任何东西，直接读 g_sortCol/g_sortAscending/g_basis 这几个全局量：
// std::sort 执行期间它们不会被修改（排序是 UI 线程上的单次同步调用），
// 因此不必包一层捕获上下文。
void sort_children_by_spec() {
    std::sort(g_childOrder.begin(), g_childOrder.end(), [](uint32_t a, uint32_t b) {
        // 比较器必须满足严格弱序。相等时不能返回 !less（那会同时
        // 认为 a<b 和 b<a），而应退回稳定的索引比较。
        bool aLess = false;
        bool bLess = false;
        if (g_sortCol == 0) {
            // 名称比较用 std::wstring 的字典序（UTF-16 码元序），
            // 对中文按码位排，不依赖当前区域设置，结果可复现。
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

// 重建当前目录的可见子项列表。名字筛选先做（字符串比较便宜，命中率低时
// 省掉后面的排序键计算）；阈值过滤随后做，并把被隐藏项的数量/字节数记下来，
// 供状态栏显示“过滤了多少”。
// 先把 g_orderDirty 清掉再判 early return：即使当前节点无效也要算“已处理”，
// 否则无效状态下每帧都会重复进入本函数。
// sz > 0 的条件是有意的：大小为 0 的空目录永远保留，否则用户会以为目录被误删。
// 这两类过滤都只影响展示，根节点的总量统计完全不受影响。
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
            // 宽字符名先转 UTF-8 再匹配：g_filter 本身就是 UTF-8，
            // 这样中文关键词按字节子串命中，无需再做宽窄转换。
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

// 深色主题，启动时一次性应用。所有尺寸都在末尾统一 ScaleAllSizes(scale)，
// 所以上面写的都是 100% DPI 下的逻辑像素；字体用同一个 scale 单独加载，
// 两者必须一致，否则控件放大而文字不变。
// 圆角只用在子窗口/控件上，主窗口保持直角以贴满客户区。
// 颜色是手工挑的低饱和蓝灰：背景足够暗让彩色占比条突出，正文亮度 0.92
// 保证对比度；系统主题变化时不会重算，需要重启才生效。
// 这里没有显式设置 AntiAliasedLines/AntiAliasedFill：它们默认开启，
// 占比条与分类小方块的圆角依赖它才不会出现锯齿。
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

    // 覆盖 ImGui 默认配色里偏亮/偏灰的项：这里只改实际用到的那些，
    // 表格行底色设为全透明，让占比条能透出到行背景上。
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

// 中文字体加载：按“显示效果优先 -> 可用性兜底”的顺序逐个尝试，
// AddFontFromFileTTF 失败返回 nullptr，循环自然跳到下一个候选：
//   msyh.ttc   = 微软雅黑（Win7+ 默认，UI 观感最好）
//   msyh.ttf   = 少数精简系统把 ttc 拆成 ttf 的形态
//   simhei.ttf = 黑体，Win7 起基本都在
//   simsun.ttc = 宋体，最后的系统字体兜底
// 全部失败才退回 ImGui 内置字体——它没有 CJK 字形，界面中文会变成方框/问号，
// 这是“能跑但不好看”的最后防线，而不是正常路径。
// 只加载简体常用字范围（约 2500 字）而不是完整 CJK，可把字体图集压到几 MB，
// 避免首次 NewFrame 时上传纹理过慢、显存占用过高。
// 字号 18*scale 与样式缩放用同一个 scale，保证文字和控件同步变大。
// 字体图集在第一次 NewFrame 时统一上传到 GPU，所以这里只负责登记字体，
// 不关心 device 是否已就绪（本函数在 ImGui 后端初始化之前调用也没问题）。
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

// 顶部工具栏：卷选择、扫描/取消、阈值滑杆、大小口径。整条用一个带边框的子窗口
// 包住，高度由“行高 + 上下内边距”算出来，保证与内部控件贴合；
// 不写死像素值，DPI 缩放后也不会裁掉文字。
// 用 SameLine(0, 18/26) 的固定间距把工具栏分成“卷 | 扫描 | 阈值 | 口径”四组，
// 组内间距小、组间间距大，靠留白表达分组，不额外画分隔线。
void draw_toolbar() {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float barH = ImGui::GetFrameHeight() + st.WindowPadding.y * 2.0f + 2.0f;
    ImGui::BeginChild("##toolbar", ImVec2(0, barH), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar);

    // AlignTextToFramePadding 让纯文本标签与同一行的输入框基线对齐，
    // 否则文字会贴着行顶，看起来比控件高半截。
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("卷");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    // 卷列表为空时给一个占位文案，避免空字符串让 combo 的命中区域塌陷。
    // 选择项只改索引，真正生效是在下次点击“扫描”时（见 start_scan）。
    const std::string currentVol =
        g_volumes.empty() ? std::string("(无)") : cs::wide_to_utf8(g_volumes[g_volumeIndex]);
    if (ImGui::BeginCombo("##vol", currentVol.c_str())) {
        for (int i = 0; i < static_cast<int>(g_volumes.size()); ++i) {
            const std::string label = cs::wide_to_utf8(g_volumes[i]);
            if (ImGui::Selectable(label.c_str(), i == g_volumeIndex)) g_volumeIndex = i;
        }
        ImGui::EndCombo();
    }

    // 扫描期间禁用“扫描”、启用“取消”，两个按钮的禁用条件互为反面。
    // 取消只置 atomic 标志，工作线程在解析循环里自行检查并尽快退出，
    // UI 不会等待它结束（结果仍由 poll_scan 收割）。
    ImGui::SameLine(0, 18);
    ImGui::BeginDisabled(g_scanning.load());
    if (ImGui::Button("  扫描  ")) start_scan();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_scanning.load());
    if (ImGui::Button("取消")) g_progress.cancel.store(true);
    ImGui::EndDisabled();

    // 阈值滑杆用 log10 映射：范围 3..10.5 对应 1KB..约 31.6GB，跨越七个数量级，
    // 线性滑杆几乎无法在 KB 段微调。拖动时反算 10^v 并夹到 >=1KB，
    // 标脏让下一帧重建列表；右侧实时显示人类可读大小，免得用户对着指数猜。
    // 显示的格式串留空，数值由旁边的 Text 提供，避免默认的“%.3f”噪声。
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

    // “逻辑”= 文件真实字节数，“占用”= 按簇取整后的磁盘分配量，两者在大文件、
    // 压缩/稀疏文件上差别明显。RadioButton 的选中状态由局部 basis 决定，
    // 最后只在值真的变化时才标脏，避免每帧都重建列表。
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

// 表头单元：拼上 ^ / v 表示当前排序列与方向，再用 Selectable 让整格可点。
// 首次点击某列时的默认方向不同——名称升序（A→Z 更自然），大小/占比降序
// （先看最占空间的），所以判断条件是 col == 0。
// 点击后立即重排已有列表，不必等下一帧的脏标记；此时 g_childOrder 已建好，
// 即使还没建（例如首帧）也只是排一个空向量，下一帧 refresh 仍会用新规则。
void header_cell(const char* label, int col) {
    ImGui::TableSetColumnIndex(col);
    const bool active = (g_sortCol == col);
    // 64 字节足够：表头最长 2 个汉字（UTF-8 6 字节）+ 箭头 + 结尾；
    // 用 snprintf 而不是字符串拼接，避免每帧为表头做一次堆分配。
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

// 单行绘制独立成函数，让 draw_list_panel 只负责“布局与循环”，
// 阅读时不必在一个函数里同时理解背景条、分类色块、交互与右键菜单。
// 把单行绘制从 draw_list_panel 拆出来，降低主列表函数的复杂度。
// 参数 cellPad 由调用方每帧取一次样式值传入，避免在逐行循环里反复查询 ImGui 样式。
void draw_child_row(uint32_t c, uint64_t total, ImDrawList* dl, float cellPad) {
    // node() 返回引用，行绘制期间不会增删节点，因此可以安全持有到函数结束；
    // 树只会在下一次 poll_scan 里被整体替换。
    const auto& n = g_tree.node(c);
    const uint64_t sz = node_size(c);
    // 用 double 算百分比而不是整数除法：uint64 的大小可达 TB 级，
    // 整数相除会把小占比直接截断成 0，进度条就没意义了。
    const double pct =
        total > 0 ? 100.0 * static_cast<double>(sz) / static_cast<double>(total) : 0.0;

    ImGui::TableNextRow();
    // 占比条必须画在整行最底层：在放置任何控件之前用当前光标位置算出矩形，
    // 否则 ImGui 的布局游标已经移动，条会错位。宽度按百分比截断；
    // 目录用蓝灰、文件用浅蓝，并统一低 alpha 以免盖住文字。
    const ImVec2 rp = ImGui::GetCursorScreenPos();
    const float rw = ImGui::GetContentRegionAvail().x;
    const float rh = ImGui::GetTextLineHeight() + cellPad * 2.0f;
    const ImU32 barCol = g_tree.isDirectory(c) ? IM_COL32(96, 130, 190, 46)
                                               : IM_COL32(130, 170, 210, 38);
    // 46/38 的低 alpha 约等于在行底色上叠 18% 不透明度：既能读出长度比例，
    // 又不会把右侧的大小/占比文字压得发灰。
    dl->AddRectFilled(rp,
                      ImVec2(rp.x + static_cast<float>(rw * pct / 100.0), rp.y + rh),
                      barCol, 3.0f);

    // 左侧 9px 小方块是分类色标：目录统一用蓝灰色（分类对目录没有意义），
    // 文件用 category_color。Dummy + SameLine 手动排 15px 缩进，
    // 让色块与后面的名称文字左对齐。
    ImGui::TableSetColumnIndex(0);
    const ImVec2 sp = ImGui::GetCursorScreenPos();
    const float ty = ImGui::GetTextLineHeight();
    const ImU32 sw = g_tree.isDirectory(c) ? IM_COL32(88, 108, 150, 255)
                                           : category_color(n.category);
    dl->AddRectFilled(ImVec2(sp.x, sp.y + (ty - 9.0f) * 0.5f),
                      ImVec2(sp.x + 9.0f, sp.y + (ty + 9.0f) * 0.5f), sw, 2.0f);
    ImGui::Dummy(ImVec2(15.0f, 0.0f));
    ImGui::SameLine(0.0f, 0.0f);

    // 名称用 Selectable 承载点击与右键：AllowOverlap 允许我们自己画的占比条
    // 不抢点击；Header 三色改成透明/浅色以贴合表格行，而不是默认的选中蓝块。
    // 悬停提示补上完整名称与精确大小（长名字在列宽里会被裁剪）。
    // 只有“有子项的目录”才下钻，避免点进空目录后只能靠面包屑返回。
    // 名称每帧转一次 UTF-8：可见行数有限（只有当前目录），这点分配可以接受；
    // 若将来要显示上万行，应改成把 UTF-8 名称缓存进树里或按行缓存。
    const std::string name = cs::wide_to_utf8(g_tree.nameString(c));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.24f, 0.34f, 0.50f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.28f, 0.40f, 0.58f, 0.70f));
    ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_AllowOverlap);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\n%s", name.c_str(), cs::human_size(sz).c_str());
    if (ImGui::IsItemClicked() && g_tree.isDirectory(c) && n.childCount > 0) navigate_to(c);
    // 三对 Push/Pop 必须严格成对且数量一致（这里写死 3），
    // 否则样式栈泄漏会污染后面所有行的文字颜色。
    ImGui::PopStyleColor(3);

    // 右键菜单三项：定位到资源管理器、复制完整路径、复制显示用的大小文本。
    // 路径在打开菜单时现算，避免为所有行缓存路径字符串占用内存；
    // 复制走 ImGui 的剪贴板封装，它会转交给 Win32 后端的 SetClipboardText。
    if (ImGui::BeginPopupContextItem()) {
        // MenuItem 只在被点击的那一帧返回 true；菜单打开期间每帧都会重建这三项。
        if (ImGui::MenuItem("在资源管理器中打开")) open_in_explorer(c);
        const std::string path = cs::wide_to_utf8(build_path(c));
        if (ImGui::MenuItem("复制路径")) ImGui::SetClipboardText(path.c_str());
        if (ImGui::MenuItem("复制大小"))
            ImGui::SetClipboardText(cs::human_size(sz).c_str());
        ImGui::EndPopup();
    }

    // 大小列与占比列只读文本。占比分母是当前目录的子树大小，因此子项之和
    // 可能小于 100%：目录自身也占大小，被阈值/筛选隐藏的项同样不计入可见行。
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(cs::human_size(sz).c_str());
    ImGui::TableSetColumnIndex(2);
    ImGui::Text("%.1f%%", pct);
}

// 主列表面板：扫描中显示进度，否则显示面包屑 + 名称筛选 + 可排序表格。
// 整个面板是一个带边框子窗口，高度由 draw_frame 按剩余空间算出。
// 子窗口 id（"list"、"##toolbar"、"##children"）都是 ImGui 用来保存滚动位置
// 等状态的键；改名等于换了一个新窗口，滚动条会跳回顶部，所以不要随手改。
void draw_list_panel(float width, float height) {
    ImGui::BeginChild("list", ImVec2(width, height), ImGuiChildFlags_Borders);

    // 扫描中只显示进度条，不显示表格：此时 g_tree 已被清空、新树还没交回，
    // 表格没有任何可画的数据；这里刻意只读 progress 里的 atomic 计数，
    // 不碰工作线程正在构造的那棵树。totalRecords 为 0 表示还没开始读目录，
    // 进度按 0 处理而不是除零。
    if (g_scanning.load()) {
        const uint64_t total = g_progress.totalRecords.load();
        const uint64_t read = g_progress.recordsRead.load();
        const float frac =
            (total > 0) ? static_cast<float>(static_cast<double>(read) / total) : 0.0f;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%llu / %llu", static_cast<unsigned long long>(read),
                      static_cast<unsigned long long>(total));
        // 文案里的 “MFT” 是固定说法；即使走的是开发用的递归扫描，
        // 这段也只作占位，不区分扫描后端（保持界面简单）。
        ImGui::TextUnformatted("正在扫描 MFT...");
        ImGui::ProgressBar(frac, ImVec2(-1, 0), buf);
        ImGui::Text("已读取 %s", cs::human_size(g_progress.bytesRead.load()).c_str());
        ImGui::EndChild();
        return;
    }

    // 面包屑：第一段是卷根，最后一段是当前目录（只显示不可点）。中间各段可点击，
    // 点击后截断路径并标脏；用 CalcTextSize 把 Selectable 宽度收紧到文字宽度，
    // 否则默认整行宽会把“斜杠后的空白”也算成点击区域，容易误触。
    if (g_path.size() > 1) {
        if (ImGui::Button("↑")) navigate_up();
    } else {
        // 已在卷根，按钮保留但禁用，避免布局左右跳动。
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
                g_orderDirty = true;  // 面包屑跳转后必须重建子项列表。
            }
            ImGui::PopStyleColor(2);
        }
    }
    ImGui::Separator();

    // 尚未扫描或扫描失败时的占位：优先展示错误，其次给出操作提示；
    // 没有管理员权限时额外用橙色提示原因，这是新手最容易卡住的地方。
    // 这里提前 EndChild 并返回，是为了让占位状态不进入后面的表格绘制，
    // 避免在 g_current 无效时对空树取 node()。
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

    // 名称筛选：InputTextWithHint 直接写进固定大小缓冲（UTF-8 字节数，非字符数），
    // 输入变化即标脏；中文按 UTF-8 子串匹配，因此不需要宽字符转换。
    // 筛选对目录与文件一视同仁（名称命中即显示），所以也能靠它按目录名定位。
    // 宽度 -1 表示撑满当前内容区。这里也是唯一的“脏检查点”：阈值拖动、
    // 面包屑跳转、排序切换、筛选输入都只置位，真正的重建只发生在这一个地方，
    // 保证每帧最多一次，且发生在遍历表格之前。
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##filter", "筛选名称…", g_filter, sizeof(g_filter)))
        g_orderDirty = true;
    if (g_orderDirty) refresh_children();

    // 百分比分母取当前目录的子树大小（按当前口径），与行内占比保持一致。
    const uint64_t total = node_size(g_current);

    // 表格标志：RowBg 提供斑马纹、ScrollY 让表头固定而内容滚动、
    // SizingFixedFit 让固定宽列严格按给定宽度排布。
    // 列宽 104/76 是 100% DPI 下的逻辑值，ImGui 会按全局 scale 一并缩放。
    // 冻结第 1 行（表头），滚动时列名始终可见。
    // 外层尺寸传 (0,0) 表示撑满当前子窗口，因此表格高度自动等于列表区高度。
    const ImGuiTableFlags tflags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##children", 3, tflags, ImVec2(0.0f, 0.0f))) {
        // 名称列拉伸，吃掉剩余宽度；大小与占比固定宽度，避免窗口变宽时
        // 数字被拉得离名称很远。三列的列号与 g_sortCol 的取值一一对应。
        ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("大小", ImGuiTableColumnFlags_WidthFixed, 104.0f);
        ImGui::TableSetupColumn("占比", ImGuiTableColumnFlags_WidthFixed, 76.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        header_cell("名称", 0);
        header_cell("大小", 1);
        header_cell("占比", 2);

        // 行绘制统一使用窗口自己的 ImDrawList：不在每行里 GetWindowDrawList，
        // 也不新建 draw list，保证占比条与文本处于同一绘制命令流，层级正确。
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float cellPad = ImGui::GetStyle().CellPadding.y;
        for (uint32_t c : g_childOrder) draw_child_row(c, total, dl, cellPad);
        // 空表也要占一行给出反馈，并区分“筛选后无匹配”与“本目录确实没有内容”，
        // 否则用户无法判断是筛选写错了还是目录本身为空。
        if (g_childOrder.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled(g_filter[0] != '\0' ? "无匹配项" : "没有可显示的项");
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

// 底部状态栏：左侧随状态变化（扫描中/错误/正常统计/未扫描），右侧是分类图例。
// 高度与工具栏用同一套公式，保证两条栏的视觉重量一致。
// 与工具栏不同，这里宽度传 0（撑满），因为右侧图例是按内容区宽度右对齐计算的。
void draw_status() {
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::BeginChild("##status", ImVec2(0, ImGui::GetFrameHeight() + st.WindowPadding.y * 2.0f),
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    if (g_scanning.load()) {
        ImGui::TextUnformatted("正在扫描...");
    } else if (!g_error.empty()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", cs::wide_to_utf8(g_error).c_str());
    } else if (g_current != cs::VolumeTree::kInvalid) {
        // 正常状态显示根总量与文件/文件夹计数。注意计数是本轮扫描的全局累计，
        // 不是当前目录的局部数量——下钻时它们保持不变，这是有意的。
        const uint64_t total = node_size(g_tree.root());
        ImGui::Text("合计 %s", cs::human_size(total).c_str());
        ImGui::SameLine(0, 16);
        ImGui::TextDisabled("文件 %llu",
                            static_cast<unsigned long long>(g_progress.fileCount.load()));
        ImGui::SameLine(0, 12);
        ImGui::TextDisabled("文件夹 %llu",
                            static_cast<unsigned long long>(g_progress.dirCount.load()));
        // 阈值过滤提示只在确实隐藏了内容时出现，避免常态占位。
        if (g_filteredCount > 0) {
            ImGui::SameLine(0, 16);
            ImGui::TextDisabled("已过滤 %llu 项 / 合计 %s",
                                static_cast<unsigned long long>(g_filteredCount),
                                cs::human_size(g_filteredBytes).c_str());
        }
        // 孤立记录（建了节点但挂不到树上）只在占比 >0.2% 时提示（orphB*500 > total
        // 即 >1/500）；否则大卷上每次都会出现一行无关紧要的告警，淹没真正的问题。
        const uint64_t orphN = g_progress.orphanNodes.load();
        const uint64_t orphB = g_progress.orphanBytes.load();
        if (orphN > 0 && orphB * 500u > total) {  // 仅在不可忽略（>0.2%）时提示
            ImGui::SameLine(0, 16);
            ImGui::TextDisabled("未连接 %llu",
                                static_cast<unsigned long long>(orphN));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("未能归入目录树的记录（约 %s）：MFT 解析的残留项",
                                  cs::human_size(orphB).c_str());
        }
    } else {
        // 还没有任何结果：用卷数量给出“程序已经就绪”的暗示。
        ImGui::TextDisabled("%zu 个 NTFS 卷，选择后点击“扫描”", g_volumes.size());
    }

    // 图例右对齐：先按文字宽度累加出总宽，只有当剩余空间足够（不会挤到左侧
    // 统计文字）时才绘制，窄窗口下宁可不显示也不重叠。
    // 从 1 遍历到 7，正好跳过 Category::Other(0)——它在图例里没有意义。
    // 色块用 AddRectFilled 手绘，再用 Dummy+SameLine 给文字让位；
    // 每项按“14px 色块 + 文字宽 + 8px 间距”估算，与累加宽度的公式保持一致。
    // 估算值比实际绘制略宽（中文标签长度不一），所以右对齐留出的空隙只多不少。
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
            // 色块与文字行高垂直居中：偏移 (ty - 9)/2，
            // 与表格里的分类小方块使用同一套几何，视觉上统一。
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

// 每帧的布局：一个铺满客户区的无边框窗口，内部依次是工具栏、列表、状态栏。
// 列表高度用“可用高度 - 状态栏 - 一次 ItemSpacing”现算，因此窗口拉伸时
// 三段始终贴合，不会出现整页滚动条或底部留白。
// 窗口标志去掉标题栏/边框/移动/缩放，让它看起来就是应用主界面本身；
// NoBringToFrontOnFocus 避免点击内部控件时窗口层级被抬起。
// SetNextWindowPos/Size 每帧都重设：窗口被拉伸或跨 DPI 显示器拖动后，
// io.DisplaySize 会变，主窗口必须跟着铺满新的客户区。
void draw_frame() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

    // 先画工具栏：它占固定高度，剩下的空间才分配给列表与状态栏。
    draw_toolbar();

    const ImGuiStyle& st = ImGui::GetStyle();
    const float statusH = ImGui::GetFrameHeight() + st.WindowPadding.y * 2.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float panelH = avail.y - statusH - st.ItemSpacing.y;

    // width 传 0 表示撑满可用宽度；高度用上面算出的 panelH。
    draw_list_panel(0.0f, panelH);
    draw_status();

    ImGui::End();
}

}  // namespace

// 入口：建窗口 -> 建 D3D11 -> 建 RTV -> 初始化 ImGui -> 主循环 -> 逆序清理。
// 任何一步失败都走同一条清理路径并返回非零，避免半初始化的对象泄漏。
// 返回的是进程退出码（0 表示正常关闭）。
int run() {
    // 必须在创建窗口之前开启 DPI 感知：否则 Windows 会对客户区做位图拉伸，
    // 拿到的 HWND 尺寸和 DPI 比例都不准，字体与控件会整体发虚。
    ImGui_ImplWin32_EnableDpiAwareness();

    // 注册窗口类：CS_CLASSDC 让系统为该类维护一个共享 DC（DX 用不到，只是惯例），
    // 光标与图标用系统默认，背景不刷（swap chain 每帧覆盖整个客户区）。
    // 返回值未检查：失败通常只表示该类已经注册过（同进程内重复调用），
    // 已存在的类照样能用来建窗口；真正的失败会由 CreateWindowW 暴露出来。
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SpaceLensWnd";
    RegisterClassExW(&wc);

    // 创建主窗口；1280x820 只是初始尺寸，之后可自由拉伸（WM_SIZE 会驱动重建 RTV）。
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"SpaceLens - 磁盘空间分析",
                              WS_OVERLAPPEDWINDOW, 80, 80, 1280, 820, nullptr, nullptr,
                              wc.hInstance, nullptr);
    // 窗口或设备任一失败：按“先建的后拆”逆序清理，再注销窗口类后退出。
    // 注意 UnregisterClassW 只在没有窗口实例时才会成功，所以先 DestroyWindow。
    if (hwnd == nullptr || !create_device(hwnd)) {
        cleanup_device();
        if (hwnd) DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    // 先建好 RTV 再 ShowWindow：否则第一帧就可能在没有渲染目标的情况下 Present。
    // 这里的失败无法用 UI 提示（界面还没起来），只能写调试日志退出。
    if (!create_rtv()) {
        dbg("create_rtv failed");
        cleanup_device();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    dbg("device + swapchain created");
    // ShowWindow 之后立刻 UpdateWindow 会强制投递一次 WM_PAINT；本程序不靠
    // WM_PAINT 绘制，但这样窗口在进入主循环前就已可见，不会先闪一下空白。
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // ImGui 上下文与后端：开启键盘导航（Tab/方向键）便于无鼠标操作；
    // DPI 比例同时作用于样式尺寸与字体大小，二者必须用同一个 scale，
    // 否则控件会随 DPI 变大而文字不变。
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // io 是引用，后端每帧会更新其中的 DisplaySize/DeltaTime/输入状态，
    // 因此只保存引用，不要把它里面的值缓存到循环外。
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    setup_style(scale);
    load_font(scale);

    // 后端初始化必须在第一次 NewFrame 之前完成；Win32 后端负责输入与光标，
    // DX11 后端负责纹理/顶点缓冲，二者共享同一个 device/context。
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    dbg("imgui dx11 initialized");

    // 探测本机 NTFS 盘符；同时检查卷句柄可打开性（即是否有管理员权限）。
    // 探测失败只影响提示文案，扫描按钮仍然可点——真正的错误会在扫描线程里
    // 以更具体的中文消息返回。
    g_volumes = cs::list_ntfs_volumes();
    g_volumeAccessible = can_open_volume();

    // 开发模式下自动开扫，省掉每次手动点按钮（无头截图脚本依赖这一点）。
    // 抓图路径同样只在这里读一次环境变量，循环里不再重复调用
    // GetEnvironmentVariableW（每次都是系统调用，帧内调用没有必要）。
    std::wstring devDir;
    if (dev_recursive_dir(devDir)) start_scan();  // 开发模式下自动开扫，用于冒烟测试
    {
        wchar_t cap[1024] = {};
        if (GetEnvironmentVariableW(L"SPACELENS_CAPTURE", cap, 1024) > 0)
            g_capturePath = cap;
    }

    // 主循环：消息泵 -> 遮挡检测 -> 尺寸处理 -> 三连 NewFrame -> 轮询扫描 ->
    // 绘制 -> 提交。顺序不能随意调整：尺寸必须在 NewFrame 之前生效，
    // poll_scan 必须在 draw_frame 之前，才能在同一帧看到刚扫描完的数据。
    // frame 只统计真正走到 Present 的迭代：被遮挡时 continue 的分支不会自增，
    // 因此“第 120 帧抓图”指的是渲染了 120 帧，而不是循环了 120 次。
    unsigned long long frame = 0;
    bool done = false;
    while (!done) {
        // 消息泵用 PeekMessage 非阻塞地抽干队列；WM_QUIT 不会进入 wnd_proc 的
        // switch（DefWindowProc 直接投递），所以必须在这里单独判断并置 done。
        // TranslateMessage 对 ImGui 不是必需的（字符输入由后端自己取），
        // 但保留它能保证将来加原生控件时的行为与常规 Win32 程序一致。
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // 窗口被完全遮挡时 Present 会返回 DXGI_STATUS_OCCLUDED。先用 PRESENT_TEST
        // 做一次无副作用试探，仍被遮挡就睡 10ms 再回到循环顶部——这样锁屏或
        // RDP 断开时 CPU 占用接近 0，而一旦恢复可见立刻继续正常渲染。
        if (g_swapOccluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        g_swapOccluded = false;

        // 尺寸变化在这里统一处理：先释放旧 RTV（它引用着即将被替换的后台缓冲），
        // 再 ResizeBuffers，最后重建 RTV。两次调用之间必须保持“没有视图引用
        // 后台缓冲”，否则 ResizeBuffers 会返回 DXGI_ERROR_INVALID_CALL。
        if (g_resizeW != 0 && g_resizeH != 0) {
            cleanup_rtv();
            const HRESULT resizeHr =
                g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = 0;
            g_resizeH = 0;
            // ResizeBuffers 失败时保留空 RTV，下面会跳过渲染而不是
            // 把空指针交给 OMSetRenderTargets/ClearRenderTargetView。
            // 这里没有处理 DXGI_ERROR_DEVICE_REMOVED（驱动重置/显卡切换），
            // 真遇到时需要重建整个设备，而不只是 RTV。
            if (FAILED(resizeHr) || !create_rtv()) dbg("resize/recreate RTV failed");
        }

        // NewFrame 顺序：先 DX11 后端（准备 GPU 侧数据），再 Win32 后端
        // （更新输入状态与鼠标光标），最后 ImGui::NewFrame 汇总成帧。
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // 每帧先收割扫描结果再绘制，保证扫描完成的那个帧就能显示完整列表。
        // 绘制阶段只读全局状态，而 poll_scan 里 move 进来的新树不会被并发访问。
        poll_scan();
        draw_frame();

        // 渲染：只有 RTV 有效时才设置渲染目标、清屏并提交绘制列表。
        // 清屏色与窗口背景一致，避免尺寸变化的瞬间露出黑边。
        ImGui::Render();
        const float clear[4] = {0.07f, 0.07f, 0.08f, 1.0f};
        if (g_rtv != nullptr) {
            g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
            g_context->ClearRenderTargetView(g_rtv, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }

        // 抓图时机定在第 120 帧：字体图集上传、样式与布局都已稳定，
        // 太早会抓到空白或半帧；抓完立即置 done，退出流程与正常关闭一致。
        // BMP 是同步写盘，只发生一次；必须留在 UI 线程、在 Present 之前完成，
        // 因为它读的是 GPU 后台缓冲，换线程反而要额外同步。
        if (!g_capturePath.empty() && frame == 120) {
            save_backbuffer_bmp(g_capturePath);
            dbg("captured back buffer");
            done = true;
        }

        // vsync 提交；Present 返回 DXGI_STATUS_OCCLUDED 时记下来，下一帧先做试探。
        // 第一个参数 1 是同步间隔（等价于一次垂直同步），远程桌面/虚拟机上
        // 可能被驱动忽略，此时循环会以 CPU 能跑的最快速度前进。
        // frame==1 与每 120 帧的日志用于确认渲染循环真的在跑（以及节点数是否合理），
        // 这是无控制台环境下唯一的运行时观测手段。
        const HRESULT hr = g_swapChain->Present(1, 0);  // 垂直同步
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

    // 退出顺序：先请求取消并 join 扫描线程，再拆后端与设备。
    // 后台线程按引用使用 g_progress；若先 DestroyContext/cleanup_device，
    // 工作线程可能还在写进度；更糟的是 future 析构时任务仍在跑，
    // 进程退出阶段会阻塞甚至触发 std::terminate。
    // 全部 get() 之后，全局对象才进入“只有主线程使用”的安全状态。
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
    // 先销毁窗口再注销窗口类：只要还有窗口实例，UnregisterClassW 就会失败。
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

}  // namespace gui
