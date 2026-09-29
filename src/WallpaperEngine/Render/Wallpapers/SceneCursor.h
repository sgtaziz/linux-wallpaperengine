#pragma once

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include "SceneTransform.h"
#include <optional>

namespace WallpaperEngine::Render::Wallpapers {

/** MouseInput uses OpenGL bottom-left pixels; SceneScript screen coordinates use top-left pixels. */
inline glm::vec2 cursorScreenPosition (const glm::dvec2& glPosition, const glm::ivec4& viewport) {
    if (viewport.z <= 0 || viewport.w <= 0) return {};
    return {
        static_cast<float> (std::clamp (glPosition.x - viewport.x, 0.0, static_cast<double> (viewport.z))),
        static_cast<float> (viewport.w - std::clamp (glPosition.y - viewport.y, 0.0,
                                                     static_cast<double> (viewport.w)))
    };
}

/** Project the visible UV through the camera onto the 2D authored z=0 plane. */
inline std::optional<glm::vec3> cursorWorldPosition (
    const glm::vec2& visibleUv, const glm::mat4& projection, float sceneWidth, float sceneHeight
) {
    if (!std::isfinite (sceneWidth) || !std::isfinite (sceneHeight)
        || sceneWidth <= 0.0f || sceneHeight <= 0.0f) return std::nullopt;
    const float determinant = glm::determinant (projection);
    if (!std::isfinite (determinant) || determinant == 0.0f) return std::nullopt;
    // The z coordinate is the projected depth of the scene's authored z=0 plane.
    const glm::vec4 planeClip = projection * glm::vec4 (0.0f, 0.0f, 0.0f, 1.0f);
    if (!std::isfinite (planeClip.z) || !std::isfinite (planeClip.w) || planeClip.w == 0.0f)
        return std::nullopt;
    // The presentation V coordinate matches the scene camera's GL clip Y.
    const glm::vec4 ndc (visibleUv.x * 2.0f - 1.0f, visibleUv.y * 2.0f - 1.0f,
                         planeClip.z / planeClip.w, 1.0f);
    const glm::vec4 projected = glm::inverse (projection) * ndc;
    if (!std::isfinite (projected.w) || projected.w == 0.0f) return std::nullopt;
    const glm::vec3 world = glm::vec3 (projected) / projected.w;
    if (!std::isfinite (world.x) || !std::isfinite (world.y) || !std::isfinite (world.z))
        return std::nullopt;
    return glm::vec3 (world.x + sceneWidth * 0.5f, sceneHeight * 0.5f - world.y, 0.0f);
}

/** Layer coordinates have their origin at the upper-left of the centered quad. */
inline std::optional<glm::vec3> cursorLocalPosition (
    const glm::vec3& authoredWorld, const glm::mat4& authoredMatrix, const glm::vec2& size,
    const glm::vec2& alignment = {}
) {
    if (!std::isfinite (size.x) || !std::isfinite (size.y) || size.x <= 0.0f || size.y <= 0.0f)
        return std::nullopt;
    const auto inverse = inverseFiniteTransform (authoredMatrix);
    if (!inverse) return std::nullopt;
    const glm::vec4 local4 = *inverse * glm::vec4 (authoredWorld, 1.0f);
    if (!std::isfinite (local4.x) || !std::isfinite (local4.y) || !std::isfinite (local4.w)
        || local4.w == 0.0f) return std::nullopt;
    const glm::vec3 local = glm::vec3 (local4) / local4.w;
    return {glm::vec3 (local.x - alignment.x + size.x * 0.5f,
                       local.y - alignment.y + size.y * 0.5f, local.z)};
}

inline std::optional<glm::vec3> cursorHitLocalPosition (
    const glm::vec3& authoredWorld, const glm::mat4& authoredMatrix, const glm::vec2& size,
    const glm::vec2& alignment = {}
) {
    const auto local = cursorLocalPosition (authoredWorld, authoredMatrix, size, alignment);
    if (!local || local->x < 0.0f || local->x > size.x || local->y < 0.0f || local->y > size.y)
        return std::nullopt;
    return local;
}

} // namespace WallpaperEngine::Render::Wallpapers
