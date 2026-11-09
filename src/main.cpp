#include <windows.h>

#include "gui/app.hpp"

// 程序的 GUI 入口（Win32 图形子系统的宽字符入口，对应 /SUBSYSTEM:WINDOWS）。
// 使用 wWinMain 而不是 main/WinMain：路径与卷标都是 UTF-16，
// 全程使用宽字符 API 才能正确处理非 ANSI 字符集的文件名。
//
// 这里刻意保持极薄：命令行参数一律不解析（本程序没有命令行界面），
// 窗口类注册、DX11 设备创建、ImGui 初始化和消息循环全部封装在 gui::run()，
// 这样入口文件不依赖任何 GUI 细节，也便于未来替换外层框架。
//
// 返回值直接作为进程退出码：0 表示正常退出，非 0 由 gui::run() 决定。
int WINAPI wWinMain(HINSTANCE /* 未用 */, HINSTANCE /* 未用 */, PWSTR /* 未用 */, int /* 未用 */) {
    return gui::run();
}
