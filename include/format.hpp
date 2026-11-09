#pragma once

// 字节数格式化工具：把 uint64_t 的原始字节数换算成 "1.23 GB" 这种便于阅读的
// 文本。它只服务于展示层（GUI 列、诊断报告），绝不参与任何大小计算——
// 统计、比较、聚合一律继续使用 uint64_t 的原始字节值。

#include <cstdint>
#include <cstdio>
#include <string>

namespace cs {

// 把字节数格式化为 "123 B" / "1.23 GB" 形式的短字符串。
//
// 约束与设计取舍：
// - 单位按 1024 进制递进（B/KB/MB/GB/TB/PB），与资源管理器和磁盘工具的
//   显示习惯一致；这里刻意不用 SI 的 1000 进制，否则同一文件在两处显示的
//   数字会对不上，容易被误判成统计错误。
// - 小于 1024 字节时输出精确整数，不显示 "0.50 KB"：小文件看真实字节数
//   比看带小数的单位更有意义，也避免了四舍五入后显示成 0 的情况。
// - 结果写入 64 字节的栈缓冲，最长形式也远小于该容量，因此不需要动态分配。
//   这让扫描线程可以高频调用它写日志而不会引入额外开销或分配失败的路径。
// - 入参和内部回退路径都保持 64 位：现代卷动辄超过 4 GiB，任何中间步骤
//   退化成 int/32 位都会溢出。
inline std::string human_size(uint64_t bytes) {
    // 下标 0 表示原始字节；每除一次 1024 前进一档，PB 是表的最后一项。
    static const char* const units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    // 仅最终展示阶段转成 double：double 有 53 位有效位，对 8 PiB 以内的
    // 数值仍能保留足够精度；累计统计仍然只用整数，避免浮点误差累积。
    double v = static_cast<double>(bytes);
    int i = 0;
    // i < 5 同时是边界保护：超过 PB 的量级继续以 PB 为单位表示，
    // 保证 units[i] 不会越界读取。
    while (v >= 1024.0 && i < 5) {
        v /= 1024.0;
        ++i;
    }
    char buf[64];
    if (i == 0) {
        // 未做除法，说明仍是字节级，直接按整数打印，保留精确值。
        // 使用 %llu 而不是 %zu，是为了在 MSVC 上也能稳定匹配 64 位无符号类型。
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    } else {
        // 两位小数是显示精度和列宽稳定之间的折中：GUI 表格的该列不会随
        // 数值变化而频繁抖动宽度。
        std::snprintf(buf, sizeof(buf), "%.2f %s", v, units[i]);
    }
    return buf;
}

}  // namespace cs
