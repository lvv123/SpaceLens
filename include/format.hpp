#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace cs {

// Human-readable byte size, e.g. "1.23 GB".
inline std::string human_size(uint64_t bytes) {
    static const char* const units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = static_cast<double>(bytes);
    int i = 0;
    while (v >= 1024.0 && i < 5) {
        v /= 1024.0;
        ++i;
    }
    char buf[64];
    if (i == 0) {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f %s", v, units[i]);
    }
    return buf;
}

}  // namespace cs
