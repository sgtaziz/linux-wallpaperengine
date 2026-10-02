#pragma once

#include "ParticleNativeRuntime.h"
#include "ParticleCore.h"
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

namespace WallpaperEngine::Render::Objects::ParticleCore {

struct BoidsParameters {
    float separationThreshold;
    float neighborThreshold;
    float maxSpeed;
    float separationFactor;
    float alignmentFactor;
    float cohesionFactor;
    uint32_t flags;
};

// Original opcode11 visits persistent four-lane blocks, including dead target
// lanes, but only nonzero lifetime markers contribute as neighbors. Later
// blocks read the velocities written by earlier blocks in this invocation.
template <typename Slot> void applyNativeBoids (
    std::vector<Slot>& slots, uint32_t highWater, uint32_t frame,
    MovementTime time, const BoidsParameters& parameters
) {
    // Original23fbc0 broadcasts interpreter param3 before each record.
    // Boids consumes the damped clock, independently of Movement's param2.
    const float movementDuration = time.damping;
    const uint32_t stride = highWater / 200u + 1u;
    const uint32_t step = stride * 4u;
    const float separation = static_cast<float> (stride) * parameters.separationFactor;
    const float alignment = (static_cast<float> (stride) * parameters.alignmentFactor) * movementDuration;
    const float cohesion = (static_cast<float> (stride) * parameters.cohesionFactor) * movementDuration;
    constexpr std::array<uint32_t, 4> rotations {0, 3, 2, 1};
    for (uint32_t block = (frame % stride) * 4u; block < highWater; block += step) {
        std::array<glm::vec3, 4> output;
        for (uint32_t lane = 0; lane < 4u; ++lane) {
            const auto& current = slots[block + lane];
            glm::vec3 repel (0), neighborVelocity (0), neighborPosition (0);
            float separationCount = 0, neighborCount = 0;
            for (uint32_t other = ((frame + block) % stride) * 4u;
                 other < highWater; other += step) {
                for (const auto rotation : rotations) {
                    const auto& neighbor = slots[other + ((lane + rotation) & 3u)];
                    const glm::vec3 delta = current.position - neighbor.position;
                    const float squared = nativeRuntimeSquaredLength (delta.x, delta.y, delta.z);
                    const float inverse = nativeRuntimeReciprocalSqrt (squared);
                    // RSQRT(0)*0 is NaN: self/coincident particles fail both
                    // strict comparisons. Do not replace this with sqrt(0).
                    const float length = inverse * squared;
                    // Native rotates XYZ/velocity, but retains the original
                    // lifetime mask lane for all four pairings in this block.
                    const bool marked = slots[other + lane].lifetime != 0.0f;
                    if (marked && length < parameters.neighborThreshold) {
                        neighborCount += 1.0f;
                        neighborVelocity += neighbor.velocity;
                        neighborPosition += neighbor.position;
                    }
                    float weight = 0.0f;
                    if (marked && squared != 0.0f && length < parameters.separationThreshold) {
                        separationCount += 1.0f;
                        weight = inverse * parameters.separationThreshold - 1.0f;
                    }
                    // Native masks the weight, then multiplies delta even
                    // when rejected: NaN coordinates still propagate via0*NaN.
                    repel += weight * delta;
                }
            }
            const float repelScale = separationCount != 0.0f
                ? (nativeRuntimeReciprocal (separationCount) * separation) * movementDuration : 0.0f;
            const float reciprocal = neighborCount != 0.0f ? nativeRuntimeReciprocal (neighborCount) : 0.0f;
            const float alignScale = neighborCount != 0.0f ? alignment : 0.0f;
            const float cohesionScale = neighborCount != 0.0f ? cohesion : 0.0f;
            glm::vec3 candidate;
            for (int axis = 0; axis < 3; ++axis) {
                const float added = ((reciprocal * neighborVelocity[axis] - current.velocity[axis]) * alignScale
                    + repelScale * repel[axis])
                    + (reciprocal * neighborPosition[axis] - current.position[axis]) * cohesionScale;
                candidate[axis] = current.velocity[axis] + added;
            }
            if ((parameters.flags & 1u) != 0) {
                const float oldSquared = nativeRuntimeSquaredLength (
                    current.velocity.x, current.velocity.y, current.velocity.z);
                const float newSquared = nativeRuntimeSquaredLength (candidate.x, candidate.y, candidate.z);
                if (nativeRuntimeMaximum (oldSquared, parameters.maxSpeed * parameters.maxSpeed) < newSquared)
                    candidate *= nativeRuntimeReciprocalSqrt (newSquared) * parameters.maxSpeed;
            }
            output[lane] = candidate;
        }
        for (uint32_t lane = 0; lane < 4u; ++lane) slots[block + lane].velocity = output[lane];
    }
}

} // namespace WallpaperEngine::Render::Objects::ParticleCore
