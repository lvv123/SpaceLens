# AGENTS.md

仅限 Windows 的 C++20 桌面应用：通过直接读取 NTFS `$MFT` 分析磁盘空间占用，
并以按大小排序的层级列表展示。需求文档：`需求文档.md`。

## 前置依赖

GUI 依赖 Dear ImGui，它以 **git submodule** 形式引入并锁定到某个 release tag：

```sh
git submodule update --init --recursive
```

如果当前网络无法克隆，请先设置代理
（`$env:HTTP_PROXY` / `$env:HTTPS_PROXY`，例如 `http://127.0.0.1:7890`）。

## 构建与测试

这里的默认 CMake 生成器是 **Visual Studio 2022**（多配置），因此需要传
`--config`。默认构建类型是 **Release**（性能优先）。

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

- 可执行文件：`build/bin/Release/spacelens.exe`（多配置生成器会多一层
  配置子目录；单配置生成器直接输出到 `build/bin`）。
- 测试**默认关闭**，需 `-DBUILD_TESTING=ON`。没有测试框架：
  `tests/unit_tests.cpp` 是手写 runner，失败时返回非零。
- 也安装了 MinGW GCC 15（`-G "MinGW Makefiles"`），但 GUI 假定使用
  MSVC/DX11，且管理员清单仅支持 MSVC。

## 目录结构

- `include/` —— 公共头文件（`ntfs.hpp`、`tree.hpp`、`scan.hpp`、`format.hpp`、
  `utf8.hpp`）。包含方式为 `"ntfs.hpp"`（include 根目录是 `include/`）。
- `src/core/` —— `spacelens_core` 静态库：MFT 解析、目录树、扫描器。
  **不依赖 UI**，因此保持可单元测试。
- `src/gui/` —— Win32 + DirectX 11 + ImGui 应用（`app.cpp`）。
- `tests/unit_tests.cpp` —— 核心测试：合成的 NTFS 记录、在内存卷镜像上做端到端
  MFT 扫描，以及在临时目录上对照递归扫描器的 oracle 测试。
- `scan_mft(VolumeReader&, ...)` 是扫描器可测试的核心；测试用 `MemoryReader`
  读取手工构造的 NTFS 镜像（`build_volume`）。新增扫描逻辑请继续走
  `VolumeReader`，以保持可测试性。
- `third_party/imgui` —— ImGui submodule（不要修改；通过更新 pin 来升级）。

源文件在 `CMakeLists.txt` 中显式列出（不使用 glob）——新增文件要手动加到对应
target。

## 运行 / 权限

- 应用会打开 `\\.\X:` 读取 MFT，这**需要管理员权限**。默认构建会嵌入
  `requireAdministrator` 清单（UAC）。
- 若要在**不提权**的情况下运行/测试 GUI（例如自动化冒烟测试），使用
  `-DSPACELENS_NO_ADMIN_MANIFEST=ON` 构建；此时 MFT 扫描会干净地失败，
  UI 显示“需要管理员权限”的提示。
- 开发捷径：设置 `SPACELENS_DEV_DIR=<目录>` 可自动对指定目录启动
  `scan_directory_recursive` 扫描，而不是 MFT 扫描。仅用于无管理员权限的开发，
  正常运行不要设置。
- `SPACELENS_LOG=<文件>` 会写一个很小的 GUI 调试日志（帧计数、节点数）——
  GUI 没有控制台时很有用。
- `SPACELENS_CAPTURE=<文件.bmp>` 会在第 120 帧把客户区渲染结果导出为 32 位
  BMP 后退出——用于无头视觉 QA（在 RDP/无头会话中从外部截取 DX 表面不可行）。
- 使用 `-DSPACELENS_TOOLS=ON` 构建还会得到 `mft_scan` / `mft_record` 诊断工具。

## 核心约定 / 易错点

- 大小始终用 `uint64_t`；绝不要用 `float`/`int` 存储。
- 硬链接：一个 `$FILE_NAME` 属性 = 一个目录项 = 一个树节点（同一条 MFT 记录可以
  对应多个节点）。这与资源管理器一致。
- 父子链接会**校验 FRN 序列号**（高 16 位），避免已回收的 MFT 记录被挂到错误的
  目录下。
- NTFS 元数据记录是 `recordNumber < 16`，但根目录（记录 5）除外；其他所有记录
  （包括 `$Recycle.Bin`）都按真实文件计入。
- 解析记录或其 `$DATA` run list 之前，始终先执行 `apply_fixups`。
- 文件大小来自**未命名的 `$DATA` 属性**（非常驻：头部 `0x28`/`0x30`；常驻：
  内容长度），回退到 `$FILE_NAME`（`0x28`/`0x30`）。`$STANDARD_INFORMATION`
  **没有**大小字段——不要从它读大小。
- 大文件/碎片文件会把 `$DATA` 放在由 `$ATTRIBUTE_LIST` 列出的扩展记录中
  （属性列表本身可能是常驻**或**非常驻的）；此时基础记录没有 `$DATA`，且其
  `$FILE_NAME` 大小为 **0**。要顺着列表找到 VCN 最低的 `$DATA` extent
  （参见 `sizeFollow*` 诊断）。这种情况下 `$FILE_NAME` 不是安全的回退来源。
- `$FILE_NAME` 的命名空间可能是 0（POSIX）或 2（DOS），不只是 Win32（1/3）。
  丢弃非 Win32 名称曾导致真实卷上整棵子树成为孤儿。每个父**记录号**选一个名称，
  优先选择 Win32 命名空间。
- 目录树是 arena 结构（`std::vector<Node>` + 索引/兄弟链 + 名称池），不是每个
  节点一个指针——大卷场景请保持这种设计。
- GUI 只展示当前层级的直接子项（逐层下钻）。大小阈值与名称筛选都只是**展示过滤**，
  不改变已统计的总大小；状态栏单独显示被阈值过滤的数量/合计。
