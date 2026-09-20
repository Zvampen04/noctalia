#pragma once

#include <algorithm>
#include <cmath>

namespace ui {

struct CarouselSlice {
    float x;
    float y;
    float width;
    float height;
    int z;
};

inline CarouselSlice carouselSlice(int index,
                                  int selected,
                                  float viewportWidth,
                                  float expandedWidth,
                                  float expandedHeight,
                                  float sliceWidth,
                                  float sliceHeight,
                                  float spacing) {
    sliceWidth = std::max(1.0f, sliceWidth);
    sliceHeight = std::max(1.0f, sliceHeight);
    expandedHeight = std::max(1.0f, expandedHeight);

    expandedWidth = std::clamp(expandedWidth, 1.0f, std::max(1.0f, viewportWidth - 2.0f * sliceWidth));
    float xCenter = (viewportWidth - expandedWidth) / 2.0f;

    float normalizedSpacing = std::max(spacing, -sliceWidth + 1.0f);
    int relative = index - selected;
    float step = sliceWidth + normalizedSpacing;

    if (relative == 0) {
        return {
            xCenter,
            0.0f,
            expandedWidth,
            expandedHeight,
            100,
        };
    }

    float x = (relative < 0)
                  ? xCenter + static_cast<float>(relative) * step
                  : xCenter + expandedWidth + normalizedSpacing + static_cast<float>(relative - 1) * step;

    return {
        x,
        (expandedHeight - sliceHeight) / 2.0f,
        sliceWidth,
        sliceHeight,
        50 - std::min(std::abs(relative), 40),
    };
}

inline bool carouselContains(float x, float y, float width, float height, float skew) {
    if (width <= 0.0f || height <= 0.0f) {
        return false;
    }
    if (x < 0.0f || x > width || y < 0.0f || y > height) {
        return false;
    }

    float clampedSkew = std::clamp(std::abs(skew), 0.0f, std::max(0.0f, width - 1.0f));

    if (skew >= 0.0f) {
        float left = clampedSkew * (1.0f - y / height);
        float right = width - clampedSkew * (y / height);
        return x >= left && x <= right;
    }

    float left = clampedSkew * (y / height);
    float right = width - clampedSkew * (1.0f - y / height);
    return x >= left && x <= right;
}

}  // namespace ui
