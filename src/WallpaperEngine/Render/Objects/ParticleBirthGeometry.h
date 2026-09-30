#pragma once

#include "WallpaperEngine/Render/Utils/NativeParticleGradientNoise.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <glm/glm.hpp>

namespace WallpaperEngine::Render::Objects::ParticleCore {

struct BetweenControlPointsState {
    float phase { 0.0f };
    float step { 1.0f / 31.0f };
};

inline float betweenControlPointsStep (float count) {
    // 1401c5490 stores 1/max(count-1, .0001), unlike the circular mapper.
    return 1.0f / std::max (count - 1.0f, 0.0001f);
}

struct BetweenControlPointsResult {
    glm::vec3 position;
    glm::vec3 velocity;
    float size;
};

inline BetweenControlPointsResult betweenControlPointsBirth (
    glm::vec3 position, glm::vec3 velocity, float size,
    const glm::vec3& start, const glm::vec3& end,
    BetweenControlPointsState& state, glm::vec2 bounds, bool mirror,
    uint32_t flags, float arcAmount, const glm::vec3& arcDirection,
    float sizeReduction, bool subtractStart) {
    // v2.8.42 14023b340 opcode 14, assembly 14023ca93..14023ce31.
    // This initializer consumes no random values. Easing uses the sequence
    // phase before bounds remapping, and the endpoints remain inclusive.
    const glm::vec3 delta = end - start;
    const float distance = std::max (std::sqrt (
        delta.x * delta.x + delta.y * delta.y + delta.z * delta.z),
        std::numeric_limits<float>::min ());
    const glm::vec3 direction = delta / distance;
    if (subtractStart) position -= start;
    const float projection = position.x * direction.x
        + position.y * direction.y + position.z * direction.z;
    glm::vec3 radial = position - direction * projection;
    const float mapped = state.phase * (bounds.y - bounds.x) + bounds.x;
    const float edge = std::abs (state.phase + state.phase - 1.0f);
    const float easing = 1.0f - std::pow (edge, 2.0f);
    if ((flags & 1u) != 0) radial *= easing;
    // Retain native multiplication order rather than replacing normalized
    // direction*distance by delta; that changes the float32 rounding.
    position = radial + ((direction * mapped) * distance + start);
    if ((flags & 8u) != 0)
        position += arcDirection * ((easing * distance) * arcAmount);
    if ((flags & 2u) != 0) velocity *= easing;
    if ((flags & 4u) != 0)
        size *= (1.0f - sizeReduction) + sizeReduction * easing;

    state.phase += state.step;
    if (state.phase > 1.0f) {
        if (mirror) {
            state.step = -state.step;
            state.phase = 1.0f - (state.phase - 1.0f);
        } else {
            state.phase = 0.0f;
        }
    } else if (state.phase < 0.0f) {
        state.phase = -state.phase;
        state.step = -state.step;
    }
    return {position, velocity, size};
}

inline std::optional<float> birthSimplexNoise2 (float x, float y) {
    // Fixed-table 2-D simplex primitive 14027b170. This is distinct from
    // seeded remap noise and the 3-D turbulence primitive.
    if (!std::isfinite (x) || !std::isfinite (y)
        || std::abs (x) >= 1.0e6f || std::abs (y) >= 1.0e6f)
        return std::nullopt;
    const float skew = (x + y) * 0.3660254f;
    const int ix = static_cast<int> (std::floor (x + skew));
    const int iy = static_cast<int> (std::floor (y + skew));
    const float unskew = static_cast<float> (ix + iy) * 0.21132487f;
    const float x0 = x - (static_cast<float> (ix) - unskew);
    const float y0 = y - (static_cast<float> (iy) - unskew);
    const bool xFirst = y0 < x0;
    const auto& table = Utils::nativeParticleGradientTable;
    const auto corner = [&] (float cx, float cy, int dx, int dy) {
        const float radius = (0.5f - cx * cx) - cy * cy;
        if (radius < 0.0f) return 0.0f;
        const uint8_t hash = table[static_cast<uint8_t> (
            ix + dx + table[static_cast<uint8_t> (iy + dy)])];
        float major = cx, minor = cy;
        if ((hash & 0x3cu) == 0u) std::swap (major, minor);
        if ((hash & 1u) != 0u) minor = -minor;
        major = (hash & 2u) == 0u ? major + major : major * -2.0f;
        return ((((major + minor) * radius) * radius) * radius) * radius;
    };
    const float first = corner (x0, y0, 0, 0);
    const float middle = corner (
        (x0 - (xFirst ? 1.0f : 0.0f)) + 0.21132487f,
        (y0 - (xFirst ? 0.0f : 1.0f)) + 0.21132487f,
        xFirst ? 1 : 0, xFirst ? 0 : 1);
    const float last = corner ((x0 - 1.0f) + 0.42264974f,
                               (y0 - 1.0f) + 0.42264974f, 1, 1);
    return (middle + first + last) * 45.23065f;
}

inline std::optional<float> birthFractalNoise2 (float x, float y, int octaves) {
    float frequency = 1.0f, amplitude = 1.0f, total = 0.0f, divisor = 0.0f;
    for (int octave = 0; octave < std::clamp (octaves, 1, 8); ++octave) {
        const auto noise = birthSimplexNoise2 (frequency * x, frequency * y);
        if (!noise) return std::nullopt;
        total += *noise * amplitude;
        divisor += amplitude;
        amplitude *= 0.5f;
        frequency += frequency;
    }
    return total / divisor;
}

inline std::optional<glm::vec3> positionOffsetBirth (
    const glm::vec3& position, float sceneTime, float scale, float distance,
    float timeScale, int octaves, const glm::vec3& directions,
    const glm::vec3& sign) {
    // 14023b340 opcode 11. Inputs and result use authored native axes; the
    // caller performs the engine's Y reflection once. No RNG draw occurs.
    const float time = timeScale * sceneTime;
    const auto x = birthFractalNoise2 (position.x * scale, time, octaves);
    const auto y = birthFractalNoise2 (time, position.y * scale, octaves);
    const auto z = birthFractalNoise2 (position.z * scale, -time, octaves);
    if (!x || !y || !z) return std::nullopt;
    glm::vec3 displacement = glm::vec3 (*x, *y, *z) * directions;
    displacement = displacement * (glm::vec3 (1.0f) - glm::abs (sign))
        + glm::abs (displacement) * sign;
    return position + displacement * distance;
}

} // namespace WallpaperEngine::Render::Objects::ParticleCore
