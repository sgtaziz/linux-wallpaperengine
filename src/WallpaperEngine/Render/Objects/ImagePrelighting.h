#pragma once

#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>

namespace WallpaperEngine::Render::Objects {

struct ImagePrelightingBasis {
    glm::mat4 world;
    glm::mat4 targetProjection;
    glm::vec2 extent;
};

// Native 1402066a0 supplies centered source-pixel vertices for prelighting.
// 140207b50 preserves the authored world matrix for shared materials, and
// scales its XY columns by logical/source dimensions only for model.instanced
// (flag 0x800, distinct from autosize 0x8). Linux allocates full-resolution
// composites; the native reduced-quality model-stack adjustment is separate.
inline ImagePrelightingBasis imagePrelightingBasis (
    const glm::mat4& authoredWorld, glm::vec2 logicalSize,
    glm::vec2 sourceSize, bool instanced
) {
    glm::mat4 world = authoredWorld;
    if (instanced)
        world = glm::scale (world,
            glm::vec3 (logicalSize.x / sourceSize.x, logicalSize.y / sourceSize.y, 1.0f));
    // The authored upper edge has negative local Y. Preserve the native
    // image-target orientation independently of the world/camera projection.
    const glm::vec2 half = sourceSize * 0.5f;
    return {world,
        glm::ortho (-half.x, half.x, half.y, -half.y, -1000.0f, 1000.0f),
        sourceSize};
}

} // namespace WallpaperEngine::Render::Objects
