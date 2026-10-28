#pragma once

#include "ParticleCollision.h"
#include "PuppetMeshParser.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <limits>
#include <span>
#include <vector>

namespace WallpaperEngine::Render::Objects::ParticleCore {

struct CollisionCapsule {
    glm::vec3 start;
    glm::vec3 direction;
    float length;
    float radius;
};

inline std::optional<PuppetSkeletonData::EmissionBound> modelCollisionFallbackBound (
    std::span<const PuppetMeshData> meshes
) {
    if (meshes.empty ()) return std::nullopt;
    glm::vec3 minimum (std::numeric_limits<float>::max ()), maximum (-std::numeric_limits<float>::max ());
    // Native2617c0 aggregates authored per-mesh boxes, never triangle bounds.
    for (const auto& mesh : meshes) {
        if (!mesh.authoredBounds) return std::nullopt;
        for (size_t axis = 0; axis < 3; ++axis) {
            const auto& raw = *mesh.authoredBounds;
            minimum[axis] = nativeRuntimeMinimum (minimum[axis], raw[axis]);
            maximum[axis] = nativeRuntimeMaximum (maximum[axis], raw[axis + 3]);
        }
    }
    if (!(minimum.x < maximum.x)) return std::nullopt;
    PuppetSkeletonData::EmissionBound bound;
    const glm::vec3 extent = (maximum - minimum) * .5f;
    const glm::vec3 center = extent + minimum;
    std::copy (glm::value_ptr (extent), glm::value_ptr (extent) + 3, bound.extent.begin ());
    const glm::mat4 matrix = glm::translate (glm::mat4 (1), center);
    std::copy (glm::value_ptr (matrix), glm::value_ptr (matrix) + 16, bound.matrix.begin ());
    return bound;
}

inline CollisionCapsule modelCollisionCapsule (glm::vec3 extent, const glm::mat4& transform) {
    // Native chooses signed axis components by squared magnitude. Equal
    // axes retain the earlier axis; radius uses the larger remaining extent.
    glm::vec3 major (extent.x, 0, 0), first (0, extent.y, 0), second (0, 0, extent.z);
    if (extent.y * extent.y > extent.x * extent.x) std::swap (major, first);
    if (extent.z * extent.z > (major.x * major.x + major.y * major.y)) std::swap (major, second);
    if ((second.x * second.x + second.y * second.y) + second.z * second.z
        <= (first.x * first.x + first.y * first.y) + first.z * first.z) second = first;
    float largest = (major.x + major.y) + major.z;
    glm::vec3 axis = major / largest;
    float halfSegment = largest - ((second.y + second.x) + second.z);
    if ((extent.y * extent.y + extent.x * extent.x) + extent.z * extent.z < 0.01f) {
        axis = {0, 1, 0};
        halfSegment = 0;
        second = glm::vec3 (0);
    }
    const glm::vec3 start (transform * glm::vec4 (axis * -halfSegment, 1));
    const glm::vec3 end (transform * glm::vec4 (axis * halfSegment, 1));
    const glm::vec3 direction (transform * glm::vec4 (axis, 0));
    const glm::vec3 radius (transform * glm::vec4 (second, 0));
    const glm::vec3 delta = end - start;
    return {start, direction * (1.0f / std::sqrt (collisionDot (direction, direction))),
            std::sqrt ((delta.x * delta.x + delta.y * delta.y) + delta.z * delta.z),
            std::sqrt (collisionDot (radius, radius))};
}

inline std::vector<CollisionCapsule> modelCollisionCapsules (
    std::span<const PuppetSkeletonData::EmissionBound> bounds,
    std::span<const glm::mat4> currentGlobals, const glm::mat4& sourceWorld,
    const glm::mat4& inverseInvocation
) {
    std::vector<CollisionCapsule> result;
    result.reserve (bounds.size ());
    const glm::mat4 source = inverseInvocation * sourceWorld;
    for (size_t index = 0; index < bounds.size (); ++index) {
        // Native substitutes identity if the corresponding pose is absent.
        const auto bone = index < currentGlobals.size () ? currentGlobals[index] : glm::mat4 (1);
        const auto& bound = bounds[index];
        result.push_back (modelCollisionCapsule (
            glm::make_vec3 (bound.extent.data ()), source * bone * glm::make_mat4 (bound.matrix.data ())));
    }
    return result;
}

inline bool collideModel (
    CollisionState& state, std::span<const CollisionCapsule> capsules,
    CollisionResponse response, float bounceFactor
) {
    bool contacted = false;
    glm::vec3 normal (0);
    float penetration = 0;
    for (const auto& capsule : capsules) {
        const glm::vec3 fromStart = state.position - capsule.start;
        const float along = nativeRuntimeMinimum (
            nativeRuntimeMaximum (collisionDot (fromStart, capsule.direction), 0), capsule.length);
        const glm::vec3 delta = state.position - (along * capsule.direction + capsule.start);
        const float squared = collisionDot (delta, delta);
        if (!(squared < capsule.radius * capsule.radius)) continue;
        const float inverse = nativeRuntimeReciprocalSqrt (squared);
        normal = inverse * delta;
        penetration = inverse * squared - capsule.radius;
        contacted = true;
    }
    // All capsules inspect the original point. Only the last penetration
    // supplies the response, and MODEL kernels never clear angular streams.
    if (contacted) collisionContact (state, normal, penetration, response, bounceFactor, false);
    return contacted;
}

inline bool modelCollisionSlotScheduled (uint32_t slot, uint32_t highWater, uint32_t frame) {
    const uint32_t phases = highWater / 10000u + 1u;
    return (slot / 4u) % phases == frame % phases;
}

} // namespace WallpaperEngine::Render::Objects::ParticleCore
