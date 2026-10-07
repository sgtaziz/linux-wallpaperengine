#pragma once

#include <array>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>

namespace WallpaperEngine::Render::Objects {

// Native draws the final step of a text object's effect chain with
// the target-pixel quad built
// straight into the scene with the object's matrices, while earlier steps use
// the 2x2 clip quad into intermediates. Vertex shaders that
// offset a_Position by g_Texture0Resolution (skew MODE 1) therefore work in
// target pixels there.
//
// Corners follow the vertex order and texture coordinates of the clip quad in
// CText::setupEffects (v = 1 on the first vertex). The text's local space is
// Y-up with the effect target's first row (v = 0) at local +Y, so the v = 1
// vertices sit at local -Y.
inline std::array<float, 18> textEffectFinalPositions (const glm::vec2& size) {
    const glm::vec2 h = size * 0.5f;
    return {-h.x, -h.y, 0.0f, -h.x, h.y, 0.0f, h.x, -h.y, 0.0f,
            h.x, -h.y, 0.0f, -h.x, h.y, 0.0f, h.x, h.y, 0.0f};
}

// Model-view-projection of the final step: the text layer matrix with the
// target box (centered at `targetCenter` in layer-local pixels) as geometry.
inline glm::mat4 textEffectFinalMatrix (const glm::mat4& layerMvp, const glm::vec2& targetCenter) {
    return layerMvp * glm::translate (glm::mat4 (1.0f), glm::vec3 (targetCenter, 0.0f));
}

} // namespace WallpaperEngine::Render::Objects
