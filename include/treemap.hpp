#pragma once

// Squarified treemap layout (Bruls, Huizing, van Wijk). Pure geometry: given a
// list of weights and a rectangle, produce one rectangle per weight whose area
// is proportional to the weight.

#include <cstdint>
#include <vector>

namespace cs {

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// Weights should be sorted in descending order for a good aspect ratio.
// Writes rects.size() == weights.size(). Zero/negative weights map to empty rects.
void squarified_treemap(const std::vector<double>& weights, const Rect& bounds,
                        std::vector<Rect>& rects);

}  // namespace cs
