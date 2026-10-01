#pragma once

#include <array>
#include <cstddef>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace WallpaperEngine::Render::Wallpapers {

// Native 1400d8300 case 0x5e uploads three vec4s for four legacy lights.
// RGB is the intensity-scaled color multiplied by radius squared. The fourth
// light's RGB occupies the W channels of the first three light rows.
inline std::array<glm::vec4, 3> legacyLightPremultipliedColors (
    const std::array<glm::vec4, 4>& colorsAndRadii
) {
    std::array<glm::vec4, 3> result {};
    const float fourthRadiusSquared = colorsAndRadii[3].w * colorsAndRadii[3].w;
    for (std::size_t row = 0; row < result.size (); ++row) {
        const auto& light = colorsAndRadii[row];
        const float radiusSquared = light.w * light.w;
        result[row] = glm::vec4 (glm::vec3 (light) * radiusSquared,
            colorsAndRadii[3][row] * fourthRadiusSquared);
    }
    return result;
}

} // namespace WallpaperEngine::Render::Wallpapers
