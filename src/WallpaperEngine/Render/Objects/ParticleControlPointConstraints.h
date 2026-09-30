#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <glm/glm.hpp>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

namespace WallpaperEngine::Render::Objects::ParticleCore {

struct ConstraintControlPoint {
    glm::vec3 position { 0.0f };
    glm::vec3 previousPosition { 0.0f };
    glm::mat3 basis { 1.0f };
};

inline float constraintReciprocalSqrt (float value) {
#if defined(__SSE__)
    return _mm_cvtss_f32 (_mm_rsqrt_ss (_mm_set_ss (value)));
#else
    return 1.0f / std::sqrt (value);
#endif
}

inline float constraintReciprocal (float value) {
#if defined(__SSE__)
    return _mm_cvtss_f32 (_mm_rcp_ss (_mm_set_ss (value)));
#else
    return 1.0f / value;
#endif
}

inline bool finiteConstraintVector (const glm::vec3& value) {
    return std::isfinite (value.x) && std::isfinite (value.y) && std::isfinite (value.z);
}

inline float constraintSquaredLength (const glm::vec3& value) {
    // Native SIMD arithmetic sums Y*Y + X*X, then Z*Z.
    return (value.y * value.y + value.x * value.x) + value.z * value.z;
}

inline std::optional<glm::vec3> maintainControlPointDistance (
    glm::vec3 position, const ConstraintControlPoint& controlPoint,
    float distance, float variableStrength, float rawDeltaTime,
    float envelopeWeight = 1.0f) {
    // Windows 2.8.42 14023fbc0 opcodes 11/33. Translation transport is
    // unconditional; the lifetime envelope affects only radial correction.
    const glm::vec3 transport = controlPoint.position - controlPoint.previousPosition;
    const glm::vec3 radial = (position + transport) - controlPoint.position;
    const float determinant = glm::determinant (controlPoint.basis);
    if (!finiteConstraintVector (position) || !finiteConstraintVector (radial)
        || !std::isfinite (determinant) || determinant == 0.0f
        || !std::isfinite (distance) || !std::isfinite (variableStrength)
        || !std::isfinite (rawDeltaTime) || !std::isfinite (envelopeWeight))
        return std::nullopt;
    const glm::mat3 inverse = glm::inverse (controlPoint.basis);
    // Native reads columns of the inverse and combines their components,
    // retaining Y + X + Z addition order for the nonuniform CP metric.
    const glm::vec3 measured {
        radial.y * inverse[1].x + radial.x * inverse[0].x + radial.z * inverse[2].x,
        radial.y * inverse[1].y + radial.x * inverse[0].y + radial.z * inverse[2].y,
        radial.y * inverse[1].z + radial.x * inverse[0].z + radial.z * inverse[2].z
    };
    const float squared = constraintSquaredLength (measured);
    // Native rsqrt(0) propagates NaNs. Retain finite transport at a coincident
    // point rather than poisoning the live particle and its child tree.
    if (!(squared > 0.0f) || !std::isfinite (squared))
        return finiteConstraintVector (position + transport)
            ? std::optional<glm::vec3> (position + transport) : std::nullopt;
    const float strength = variableStrength == 0.0f ? 1.0f
        : std::clamp (variableStrength * rawDeltaTime, 0.0f, 1.0f);
    const float amount = (constraintReciprocalSqrt (squared) * distance - 1.0f)
        * strength * envelopeWeight;
    const glm::vec3 result = (radial * amount + transport) + position;
    return finiteConstraintVector (result) ? std::optional<glm::vec3> (result) : std::nullopt;
}

inline glm::vec3 maintainBetweenControlPoints (
    glm::vec3 position, const ConstraintControlPoint& start,
    const ConstraintControlPoint& end, float envelopeWeight = 1.0f) {
    // 14023fbc0 opcodes 12/34: carry the old segment's axial coordinate
    // onto the new segment; its perpendicular displacement is retained.
    const glm::vec3 current = end.position - start.position;
    const glm::vec3 previous = end.previousPosition - start.previousPosition;
    const float currentSquared = constraintSquaredLength (current);
    const float previousSquared = constraintSquaredLength (previous);
    constexpr float minimumSquared = 1.4210855e-14f;
    if (!(currentSquared > minimumSquared && previousSquared > minimumSquared)
        || !std::isfinite (currentSquared) || !std::isfinite (previousSquared)
        || !finiteConstraintVector (position) || !std::isfinite (envelopeWeight))
        return position;
    const float currentInverseLength = constraintReciprocalSqrt (currentSquared);
    const float previousInverseLength = constraintReciprocalSqrt (previousSquared);
    const glm::vec3 relative = position - start.previousPosition;
    const float projection = (relative.y * previousInverseLength * previous.y
        + relative.x * previousInverseLength * previous.x)
        + relative.z * previousInverseLength * previous.z;
    const float coordinate = std::clamp (projection * previousInverseLength, 0.0f, 1.0f);
    const float mapped = coordinate * currentInverseLength * currentSquared;
    const glm::vec3 correction = (mapped * (current * currentInverseLength)
        - (projection * previousInverseLength) * previous)
        + (start.position - start.previousPosition);
    const glm::vec3 result = correction * envelopeWeight + position;
    return finiteConstraintVector (result) ? result : position;
}

inline float movementNearControlPointMultiplier (
    glm::vec3 position, glm::vec3 center, float innerDistance, float outerDistance,
    float innerReduction, float outerReduction, float rawDeltaTime,
    float envelopeWeight = 1.0f) {
    // 1401c5490 packs native rcpps and equal-value fallbacks. 14023fbc0
    // opcodes 13/35 clamp per-pass reduction BEFORE the lifetime envelope.
    const glm::vec3 radial = position - center;
    const float squared = constraintSquaredLength (radial);
    if (!finiteConstraintVector (radial) || !std::isfinite (squared)
        || !std::isfinite (innerDistance) || !std::isfinite (outerDistance)
        || !std::isfinite (innerReduction) || !std::isfinite (outerReduction)
        || !std::isfinite (rawDeltaTime) || !std::isfinite (envelopeWeight))
        return 1.0f;
    const float distance = squared > 0.0f ? constraintReciprocalSqrt (squared) * squared : 0.0f;
    const float reciprocal = innerDistance == outerDistance ? 1.0f
        : constraintReciprocal (outerDistance - innerDistance);
    const float interpolation = std::clamp ((distance - innerDistance) * reciprocal, 0.0f, 1.0f);
    const float delta = innerReduction == outerReduction ? 1.0f : outerReduction - innerReduction;
    const float reduction = std::clamp ((interpolation * delta + innerReduction)
        * rawDeltaTime, 0.0f, 1.0f);
    return 1.0f - reduction * envelopeWeight;
}

} // namespace WallpaperEngine::Render::Objects::ParticleCore
