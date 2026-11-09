#pragma once

// 应用版本号的唯一来源。
//
// 以头文件常量而不是编译宏提供，便于 GUI 在「关于」信息或标题栏里直接引用
// （当前 app.cpp 尚未引用，属于预留的单一事实来源）。
// 采用语义化版本：主版本.次版本.修订号；发布前手动修改，不做自动生成，
// 避免构建脚本与源码里的版本漂移。
namespace spacelens {

// inline 变量：多个翻译单元包含本头文件时共享同一实体，不会违反 ODR。
// 存成 const char*（而不是 std::string）是为了零初始化开销，可直接拼进字符串。
inline constexpr const char* kVersion = "0.1.0";

}  // namespace spacelens
