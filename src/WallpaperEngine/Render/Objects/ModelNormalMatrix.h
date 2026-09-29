#pragma once

#include <cmath>
#include <glm/glm.hpp>
#include <optional>

namespace WallpaperEngine::Render::Objects {

// Native 1400d8300 removes the length of each model-stack basis column.
// This differs from inverse-transpose for nonuniform scale plus rotation or shear.
inline std::optional<glm::mat3> modelNormalMatrix (const glm::mat4& world) {
    glm::mat3 normal (world);
    for (int column = 0; column < 3; ++column) {
        const float length = glm::length (normal[column]);
        if (!std::isfinite (length)) return std::nullopt;
        // Native fast inverse-square-root multiplies a zero basis by a finite
        // estimate, leaving that column zero; planar models still draw.
        if (length == 0.0f) continue;
        normal[column] /= length;
    }
    return normal;
}

} // namespace WallpaperEngine::Render::Objects
