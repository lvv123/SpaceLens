#pragma once

// GUI 层的唯一对外接口。入口文件只认识这一个函数，
// 其余实现细节（Win32 窗口、DX11、ImGui、扫描线程）都封装在 app.cpp 内。
namespace gui {
// 创建窗口、初始化渲染后端并运行主循环，返回进程退出码。
//
// 调用约束：
// - 必须在进程主线程调用：Win32 消息队列和 DX11 都绑定创建它们的线程；
// - 该函数是阻塞的，直到用户关闭窗口才返回，返回后即可直接退出进程；
// - 没有参数，运行期行为通过环境变量调整（SPACELENS_LOG 调试日志、
//   SPACELENS_CAPTURE 截帧、SPACELENS_DEV_DIR 用目录扫描代替 MFT 扫描）；
// - app.cpp 里存在全局状态，因此同一进程只应调用一次。
int run();
}  // namespace gui
