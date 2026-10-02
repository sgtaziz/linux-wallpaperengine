#pragma once

#include <array>

namespace WallpaperEngine::Render::Objects {
struct ImageQuadUV {
    float left;
    float bottom;
    float right;
    float top;

    [[nodiscard]] std::array<float, 12> triangles () const {
        return {left, top, left, bottom, right, top,
                right, top, left, bottom, right, bottom};
    }
};

// Native 1402066a0 selects centered flag3 for the direct ordinary quad,
// excluding model mask0x226. 1401ede30 flag2 moves each edge by .15 of
// an allocated source texel, independently of logical image dimensions.
inline ImageQuadUV directImageQuadUV (
    float maxU, float maxV, int allocatedWidth, int allocatedHeight,
    bool fullscreen, bool nopadding, bool passthrough, bool solidlayer) {
    if (fullscreen || nopadding || passthrough || solidlayer
        || allocatedWidth <= 0 || allocatedHeight <= 0)
        return {0.0f, 0.0f, maxU, maxV};
    const float insetU = 0.15f / static_cast<float> (allocatedWidth);
    const float insetV = 0.15f / static_cast<float> (allocatedHeight);
    return {insetU, insetV, maxU - insetU, maxV - insetV};
}

// Native's separate +2e8 presentation quad ignores nopadding, unlike its
// direct +490 quad. Effect/required source loading forces the UV extent to
// unity (140209360); the inset still uses the original source allocation.
inline ImageQuadUV compositeImageQuadUV (
    int allocatedWidth, int allocatedHeight, bool fullscreen, bool passthrough, bool solidlayer) {
    return directImageQuadUV (1.0f, 1.0f, allocatedWidth, allocatedHeight,
                              fullscreen, false, passthrough, solidlayer);
}
}
