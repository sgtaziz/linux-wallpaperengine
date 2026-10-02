#pragma once

#include "ParticleNativeRuntime.h"
#include <array>
#include <glm/glm.hpp>

namespace WallpaperEngine::Render::Objects::ParticleCore {

enum class CollisionResponse { Bounce, Slide, Stop, Delete };

struct CollisionState {
    glm::vec3 position;
    glm::vec3 velocity;
    glm::vec3 angularVelocity;
    float age;
    float lifetime;
};

inline float collisionDot (const glm::vec3& a, const glm::vec3& b) {
    return (a.y * b.y + a.x * b.x) + a.z * b.z;
}

inline glm::vec3 collisionUnit (glm::vec3 value) {
    // Factory 14005eb80 uses scalar sqrt/divide, unlike the sphere kernel.
    return value * (1.0f / std::sqrt ((value.x * value.x + value.y * value.y) + value.z * value.z));
}

inline void collisionContact (
    CollisionState& state, const glm::vec3& normal, float penetration,
    CollisionResponse response, float bounceFactor, bool clearAngular
) {
    if (response == CollisionResponse::Delete) state.age = state.lifetime;
    else {
        const glm::vec3 velocity = state.velocity;
        state.position -= penetration * normal;
        if (response == CollisionResponse::Stop) state.velocity = glm::vec3 (0);
        else {
            const float amount = collisionDot (normal, velocity)
                * (response == CollisionResponse::Slide ? -1.0f : -1.0f - bounceFactor);
            state.velocity = amount * normal + velocity;
        }
    }
    if (clearAngular) state.angularVelocity = glm::vec3 (0);
}

inline bool collidePlane (
    CollisionState& state, const glm::vec3& normal, float distance,
    CollisionResponse response, float bounceFactor, bool clearAngular = false
) {
    const float measured = collisionDot (state.position, normal);
    if (!(measured < distance)) return false;
    collisionContact (state, normal, measured - distance, response, bounceFactor, clearAngular);
    return true;
}

inline bool collideSphere (
    CollisionState& state, const glm::vec3& origin, float radius,
    CollisionResponse response, float bounceFactor, bool clearAngular = false
) {
    const glm::vec3 delta = state.position - origin;
    const float squared = collisionDot (delta, delta);
    if (!(squared < radius * radius)) return false;
    const float inverse = nativeRuntimeReciprocalSqrt (squared);
    const glm::vec3 normal = inverse * delta;
    // Preserve the original approximate length/normal and subtraction order;
    // origin + radius*normal is not the same native projection.
    collisionContact (state, normal, inverse * squared - radius,
                      response, bounceFactor, clearAngular);
    return true;
}

struct CollisionQuad {
    glm::vec3 origin;
    glm::vec3 normal;
    glm::vec3 forward;
    glm::vec3 right;
    glm::vec2 halfSize;
};

inline CollisionQuad collisionQuad (
    const glm::vec3& origin, glm::vec3 normal, glm::vec3 forward, glm::vec2 size
) {
    normal = collisionUnit (normal);
    forward = collisionUnit (forward);
    const glm::vec3 right = collisionUnit (glm::cross (normal, forward));
    // The compiled record puts height on forward and width on right.
    return {origin, normal, collisionUnit (glm::cross (right, normal)), right,
            {size.y * 0.5f, size.x * 0.5f}};
}

inline bool collideQuad (
    CollisionState& state, const glm::vec3& previous, const CollisionQuad& quad,
    CollisionResponse response, float bounceFactor, bool clearAngular = false
) {
    const glm::vec3 currentDelta = state.position - quad.origin;
    const float current = collisionDot (currentDelta, quad.normal);
    const float old = collisionDot (previous - quad.origin, quad.normal);
    if (!(old > 0.0f && current <= 0.0f
          && std::abs (collisionDot (currentDelta, quad.forward)) < quad.halfSize.x
          && std::abs (collisionDot (currentDelta, quad.right)) < quad.halfSize.y)) return false;
    collisionContact (state, quad.normal, current * 1.05f, response, bounceFactor, clearAngular);
    return true;
}

struct CollisionBounds {
    std::array<glm::vec3, 4> normals {{{1, 0, 0}, {0, 1, 0}, {-1, 0, 0}, {0, -1, 0}}};
    std::array<float, 4> distances;
};

inline CollisionBounds collisionBounds (glm::vec2 canvas, const glm::mat4& inverseStack) {
    CollisionBounds result;
    const glm::vec3 minimum (inverseStack * glm::vec4 (0, 0, 0, 1));
    const glm::vec3 maximum (inverseStack * glm::vec4 (canvas, 0, 1));
    for (size_t i = 0; i < 4; ++i) {
        result.normals[i] = glm::vec3 (inverseStack * glm::vec4 (result.normals[i], 0));
        const auto& point = i < 2 ? minimum : maximum;
        const auto& normal = result.normals[i];
        // Native 14005f070 scalar factory order differs from SIMD contact dot.
        result.distances[i] = (point.x * normal.x + point.y * normal.y) + point.z * normal.z;
    }
    return result;
}

inline bool collideBounds (
    CollisionState& state, const CollisionBounds& bounds,
    CollisionResponse response, float bounceFactor
) {
    int selected = -1;
    float penetration = 0;
    // Native dedicated kernels select the last violated plane from the
    // original point, then apply ONE response. Corners are not four contacts.
    for (size_t i = 0; i < 4; ++i) {
        const float measured = collisionDot (state.position, bounds.normals[i]);
        if (measured < bounds.distances[i]) {
            selected = static_cast<int> (i);
            penetration = measured - bounds.distances[i];
        }
    }
    if (selected < 0) return false;
    // The bounds kernels never touch the angular streams, regardless of flags.
    collisionContact (state, bounds.normals[selected], penetration, response, bounceFactor, false);
    return true;
}

} // namespace WallpaperEngine::Render::Objects::ParticleCore
