#include "treemap.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cs {
namespace {

double worst_ratio(const std::vector<double>& row, double shortSide) {
    double sum = 0.0;
    double mx = 0.0;
    double mn = row.empty() ? 0.0 : row[0];
    for (double v : row) {
        sum += v;
        mx = std::max(mx, v);
        mn = std::min(mn, v);
    }
    if (sum <= 0.0 || shortSide <= 0.0) return std::numeric_limits<double>::max();
    if (mn <= 0.0) mn = 1e-12;
    const double s2 = sum * sum;
    const double w2 = shortSide * shortSide;
    return std::max((w2 * mx) / s2, s2 / (w2 * mn));
}

}  // namespace

void squarified_treemap(const std::vector<double>& weights, const Rect& bounds,
                        std::vector<Rect>& rects) {
    const size_t n = weights.size();
    rects.assign(n, Rect{});

    double total = 0.0;
    for (double v : weights) total += std::max(0.0, v);
    if (total <= 0.0 || bounds.w <= 0.0f || bounds.h <= 0.0f) return;

    const double area = static_cast<double>(bounds.w) * static_cast<double>(bounds.h);
    const double scale = area / total;

    std::vector<double> areas(n);
    for (size_t i = 0; i < n; ++i) areas[i] = std::max(0.0, weights[i]) * scale;

    Rect free = bounds;
    std::vector<double> row;
    size_t start = 0;
    size_t i = 0;
    while (i < n) {
        const double shortSide = std::min<double>(free.w, free.h);
        if (!row.empty()) {
            row.push_back(areas[i]);
            const double next = worst_ratio(row, shortSide);
            row.pop_back();
            if (next > worst_ratio(row, shortSide)) {
                // finalize current row
                double s = 0.0;
                for (double v : row) s += v;
                if (free.w >= free.h) {
                    const double thickness = (free.h > 0.0) ? s / free.h : 0.0;
                    double y = free.y;
                    for (size_t k = 0; k < row.size(); ++k) {
                        const double h = (thickness > 0.0) ? row[k] / thickness : 0.0;
                        rects[start + k] = Rect{free.x, static_cast<float>(y),
                                                static_cast<float>(thickness), static_cast<float>(h)};
                        y += h;
                    }
                    free.x += static_cast<float>(thickness);
                    free.w -= static_cast<float>(thickness);
                } else {
                    const double thickness = (free.w > 0.0) ? s / free.w : 0.0;
                    double x = free.x;
                    for (size_t k = 0; k < row.size(); ++k) {
                        const double wdt = (thickness > 0.0) ? row[k] / thickness : 0.0;
                        rects[start + k] = Rect{static_cast<float>(x), free.y,
                                                static_cast<float>(wdt), static_cast<float>(thickness)};
                        x += wdt;
                    }
                    free.y += static_cast<float>(thickness);
                    free.h -= static_cast<float>(thickness);
                }
                start += row.size();
                row.clear();
                continue;
            }
        }
        row.push_back(areas[i]);
        ++i;
    }

    if (!row.empty()) {
        double s = 0.0;
        for (double v : row) s += v;
        if (free.w >= free.h) {
            const double thickness = (free.h > 0.0) ? s / free.h : 0.0;
            double y = free.y;
            for (size_t k = 0; k < row.size(); ++k) {
                const double h = (thickness > 0.0) ? row[k] / thickness : 0.0;
                rects[start + k] = Rect{free.x, static_cast<float>(y),
                                        static_cast<float>(thickness), static_cast<float>(h)};
                y += h;
            }
        } else {
            const double thickness = (free.w > 0.0) ? s / free.w : 0.0;
            double x = free.x;
            for (size_t k = 0; k < row.size(); ++k) {
                const double wdt = (thickness > 0.0) ? row[k] / thickness : 0.0;
                rects[start + k] = Rect{static_cast<float>(x), free.y,
                                        static_cast<float>(wdt), static_cast<float>(thickness)};
                x += wdt;
            }
        }
    }
}

}  // namespace cs
