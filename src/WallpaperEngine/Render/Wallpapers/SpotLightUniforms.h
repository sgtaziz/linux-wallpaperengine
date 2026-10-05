#pragma once

#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace WallpaperEngine::Render::Wallpapers {
struct SpotLightUniforms {
    glm::vec4 color;
    glm::vec4 origin;
    glm::vec4 direction;
    glm::vec4 exponent;
};

// Native packs plain spot lights from the authored node matrix.
// Cones are degrees; direction is column zero, without normalizing its scale.
inline SpotLightUniforms packSpotLightUniforms (
    const glm::mat4& authoredMatrix, const glm::vec3& color, float intensity,
    float radius, float exponent, float innerCone, float outerCone
) {
    return {
        glm::vec4 (color * intensity, radius),
        glm::vec4 (glm::vec3 (authoredMatrix[3]), std::cos (glm::radians (innerCone))),
        glm::vec4 (glm::vec3 (authoredMatrix[0]), std::cos (glm::radians (outerCone))),
        glm::vec4 (exponent, 0.0f, 0.0f, 0.0f),
    };
}
struct TubeLightUniforms {
    glm::vec4 color;
    glm::vec4 originA;
    glm::vec4 originB;
};

// Native packs a tube from its
// authored node matrix: color*intensity with radius in w, segment start at
// the matrix translation with exponent in w, and segment end at the matrix
// applied to the local control point (w = 1), stored with w = 0.
inline TubeLightUniforms packTubeLightUniforms (
    const glm::mat4& authoredMatrix, const glm::vec3& color, float intensity,
    float radius, float exponent, const glm::vec3& controlPoint
) {
    return {
        glm::vec4 (color * intensity, radius),
        glm::vec4 (glm::vec3 (authoredMatrix[3]), exponent),
        glm::vec4 (glm::vec3 (authoredMatrix * glm::vec4 (controlPoint, 1.0f)), 0.0f),
    };
}
} // namespace WallpaperEngine::Render::Wallpapers
