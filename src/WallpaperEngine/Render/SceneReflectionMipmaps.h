#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>

namespace WallpaperEngine::Render {
// Native 1400d2c60/1400d3500 reserve the last three power-of-two
// reductions. This is the number of levels, not the largest LOD index.
inline uint32_t sceneReflectionMipLevels (uint32_t width, uint32_t height) {
    const auto exponent = [] (uint32_t size) {
        return std::bit_width (std::max (2u, size) - 1u);
    };
    const auto shorter = std::min (exponent (width), exponent (height));
    return shorter > 3 ? shorter - 3 : 1;
}
} // namespace WallpaperEngine::Render
