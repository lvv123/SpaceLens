#pragma once

// 宽字符（UTF-16）到 UTF-8 的转换工具。
//
// 存在的理由：Win32 文件 API 给的是 UTF-16（wchar_t，Windows 上是 2 字节），
// 而 ImGui 只接受 UTF-8 的 char*。因此转换必须发生在「显示」这一层，
// 树结构内部始终保存原始的 wstring，避免把有损转换的结果写回数据模型。
//
// 注意本文件只做单向转换：UTF-8 -> UTF-16 请直接用 Win32 的
// MultiByteToWideChar，不要在这里自行实现。

#include <string>

#include <windows.h>

namespace cs {

// 把 UTF-16 字符串转换为 UTF-8（给 ImGui 用，它期望 UTF-8）。
//
// 实现是标准的两趟调用：第一趟传 nullptr 只问所需字节数，第二趟才真正写入。
// 这样无需猜测长度，也不会因为缓冲偏小而静默截断。
// - 空串直接返回 {}，省掉两次 API 调用。
// - 第一趟失败（n <= 0）也返回空串：调用方展示空名称比展示乱码更安全。
// - 返回的 std::string 不带 WideCharToMultiByte 写入的结尾 NUL，
//   但 std::string 自身保证 c_str() 以 NUL 结束，可直接传给 ImGui。
inline std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    // 第一趟：只计算目标字节数（不包含结尾 NUL）。
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    // 预先按精确长度分配并填满 '\0'；C++17 起 data() 可写，
    // 因此可以第二趟直接写进去，省掉一次拷贝。
    std::string s(static_cast<size_t>(n), '\0');
    // 第二趟：真正转换。这里不检查返回值，因为同一个输入在两趟之间不会变化，
    // 第一趟成功就说明长度足够。
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

}  // namespace cs
