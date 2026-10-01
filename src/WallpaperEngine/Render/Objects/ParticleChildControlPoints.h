#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <optional>

namespace WallpaperEngine::Render::Objects::ParticleCore {

// Native 14022a580 copies only particle XYZ into child CP translations.
// S is the parent's current scene-stack top, before pushing the child node.
// Equal preset spaces retain XYZ; local->world uses S, world->local its inverse.
inline std::optional<glm::vec3> childParticleControlPointPosition (
    const glm::vec3& position, bool parentWorld, bool childWorld,
    const glm::mat4& parentStack, const std::optional<glm::mat4>& inverseParentStack
) {
    if (parentWorld == childWorld) return position;
    if (parentWorld && !inverseParentStack) return std::nullopt;
    const glm::mat4& matrix = parentWorld ? *inverseParentStack : parentStack;
    // Preserve the native scalar XYZ multiply/add order, without touching
    // the destination's orientation, scale, or any other CP values.
    return glm::vec3 {
        matrix[0][0] * position.x + matrix[1][0] * position.y
            + matrix[2][0] * position.z + matrix[3][0],
        matrix[0][1] * position.x + matrix[1][1] * position.y
            + matrix[2][1] * position.z + matrix[3][1],
        matrix[0][2] * position.x + matrix[1][2] * position.y
            + matrix[2][2] * position.z + matrix[3][2],
    };
}

} // namespace WallpaperEngine::Render::Objects::ParticleCore
