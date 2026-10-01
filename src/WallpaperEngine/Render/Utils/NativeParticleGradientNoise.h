#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace WallpaperEngine::Render::Utils {

// wallpaper64.exe v2.8.42 .rdata 140484f40, read by 14027b090.
inline constexpr std::array<uint8_t, 256> nativeParticleGradientTable {
    0x97, 0xa0, 0x89, 0x5b, 0x5a, 0x0f, 0x83, 0x0d, 0xc9, 0x5f, 0x60, 0x35, 0xc2, 0xe9, 0x07, 0xe1,
    0x8c, 0x24, 0x67, 0x1e, 0x45, 0x8e, 0x08, 0x63, 0x25, 0xf0, 0x15, 0x0a, 0x17, 0xbe, 0x06, 0x94,
    0xf7, 0x78, 0xea, 0x4b, 0x00, 0x1a, 0xc5, 0x3e, 0x5e, 0xfc, 0xdb, 0xcb, 0x75, 0x23, 0x0b, 0x20,
    0x39, 0xb1, 0x21, 0x58, 0xed, 0x95, 0x38, 0x57, 0xae, 0x14, 0x7d, 0x88, 0xab, 0xa8, 0x44, 0xaf,
    0x4a, 0xa5, 0x47, 0x86, 0x8b, 0x30, 0x1b, 0xa6, 0x4d, 0x92, 0x9e, 0xe7, 0x53, 0x6f, 0xe5, 0x7a,
    0x3c, 0xd3, 0x85, 0xe6, 0xdc, 0x69, 0x5c, 0x29, 0x37, 0x2e, 0xf5, 0x28, 0xf4, 0x66, 0x8f, 0x36,
    0x41, 0x19, 0x3f, 0xa1, 0x01, 0xd8, 0x50, 0x49, 0xd1, 0x4c, 0x84, 0xbb, 0xd0, 0x59, 0x12, 0xa9,
    0xc8, 0xc4, 0x87, 0x82, 0x74, 0xbc, 0x9f, 0x56, 0xa4, 0x64, 0x6d, 0xc6, 0xad, 0xba, 0x03, 0x40,
    0x34, 0xd9, 0xe2, 0xfa, 0x7c, 0x7b, 0x05, 0xca, 0x26, 0x93, 0x76, 0x7e, 0xff, 0x52, 0x55, 0xd4,
    0xcf, 0xce, 0x3b, 0xe3, 0x2f, 0x10, 0x3a, 0x11, 0xb6, 0xbd, 0x1c, 0x2a, 0xdf, 0xb7, 0xaa, 0xd5,
    0x77, 0xf8, 0x98, 0x02, 0x2c, 0x9a, 0xa3, 0x46, 0xdd, 0x99, 0x65, 0x9b, 0xa7, 0x2b, 0xac, 0x09,
    0x81, 0x16, 0x27, 0xfd, 0x13, 0x62, 0x6c, 0x6e, 0x4f, 0x71, 0xe0, 0xe8, 0xb2, 0xb9, 0x70, 0x68,
    0xda, 0xf6, 0x61, 0xe4, 0xfb, 0x22, 0xf2, 0xc1, 0xee, 0xd2, 0x90, 0x0c, 0xbf, 0xb3, 0xa2, 0xf1,
    0x51, 0x33, 0x91, 0xeb, 0xf9, 0x0e, 0xef, 0x6b, 0x31, 0xc0, 0xd6, 0x1f, 0xb5, 0xc7, 0x6a, 0x9d,
    0xb8, 0x54, 0xcc, 0xb0, 0x73, 0x79, 0x32, 0x2d, 0x7f, 0x04, 0x96, 0xfe, 0x8a, 0xec, 0xcd, 0x5d,
    0xde, 0x72, 0x43, 0x1d, 0x18, 0x48, 0xf3, 0x8d, 0x80, 0xc3, 0x4e, 0x42, 0xd7, 0x3d, 0x9c, 0xb4,
};

// Birth remap transforms 5/6 use 14027b090 with unchecked IEEE results.
// Emulate CVTTSS2SI's masked-invalid sentinel without an undefined C++ cast,
// then perform the native wrapping predecessor and low-byte table lookup.
inline float nativeBirthParticleGradientNoise (float coordinate) {
    const int32_t truncated = std::isfinite (coordinate)
        && coordinate >= -2147483648.0f && coordinate < 2147483648.0f
        ? static_cast<int32_t> (coordinate) : std::numeric_limits<int32_t>::min ();
    const int32_t cell = !(static_cast<float> (truncated) > coordinate) ? truncated
        : std::bit_cast<int32_t> (static_cast<uint32_t> (truncated) - 1u);
    const uint8_t index = static_cast<uint8_t> (cell);
    const float fraction = coordinate - static_cast<float> (cell);
    const float previous = fraction - 1.0f;
    const auto gradient = [] (uint8_t hash) {
        const float magnitude = static_cast<float> ((hash & 7u) + 1u);
        return (hash & 8u) ? -magnitude : magnitude;
    };
    float a0 = 1.0f - fraction * fraction;
    a0 *= a0;
    a0 *= a0;
    float a1 = 1.0f - previous * previous;
    a1 *= a1;
    a1 *= a1;
    return ((gradient (nativeParticleGradientTable[static_cast<uint8_t> (index + 1u)]) * previous) * a1
          + (gradient (nativeParticleGradientTable[index]) * fraction) * a0) * 0.395f;
}

// 14027b4b0 samples frequency * original coordinate each iteration, then
// divides the sequential weighted sum by its amplitude sum. Count zero
// deliberately returns 0/0; the parser bounds the authored count to 0..32.
inline float nativeBirthParticleFBMNoise (float coordinate, float frequency, int count) {
    float total = 0.0f, amplitudeSum = 0.0f, amplitude = 1.0f;
    for (int octave = 0; octave < count; ++octave) {
        total += nativeBirthParticleGradientNoise (frequency * coordinate) * amplitude;
        amplitudeSum += amplitude;
        frequency *= 2.0f;
        amplitude *= 0.5f;
    }
    return total / amplitudeSum;
}

// Native 14027b090 is one-dimensional gradient noise, unrelated to the
// 3D simplex primitive used by the turbulence operator.
inline std::optional<float> nativeParticleGradientNoise (float coordinate) {
    if (!std::isfinite (coordinate) || std::abs (coordinate) >= 1.0e6f)
        return std::nullopt; // preserve the accepted bounded opcode-9 evaluation policy
    const int cell = static_cast<int> (std::floor (coordinate));
    const float fraction = coordinate - static_cast<float> (cell);
    const float previous = fraction - 1.0f;
    const auto gradient = [] (uint8_t hash) {
        const float magnitude = static_cast<float> ((hash & 7u) + 1u);
        return (hash & 8u) ? -magnitude : magnitude;
    };
    const float g0 = gradient (nativeParticleGradientTable[static_cast<uint8_t> (cell)]);
    const float g1 = gradient (nativeParticleGradientTable[static_cast<uint8_t> (cell + 1)]);
    float a0 = 1.0f - fraction * fraction;
    a0 *= a0;
    a0 *= a0;
    float a1 = 1.0f - previous * previous;
    a1 *= a1;
    a1 *= a1;
    return ((g1 * previous) * a1 + (g0 * fraction) * a0) * 0.395f;
}

// Native initializer opcode 9: phase and speed each consume one independent
// scene MT draw. Work in authored XYZ; callers reflect the result into Linux
// particle storage. No direction normalization or XY projection occurs here.
inline std::optional<glm::vec3> nativeTurbulentBirthVelocity (
    float phaseRandom, float speedRandom, float phaseMin, float phaseDelta,
    float speedMin, float speedDelta, float sceneTime, float timeScale,
    float scale, float offset, glm::vec3 forward, glm::vec3 right) {
    const float coordinate = ((phaseRandom * phaseDelta + phaseMin) + sceneTime) * timeScale;
    const auto noise = nativeParticleGradientNoise (coordinate);
    if (!noise) return std::nullopt;
    const float angle = ((*noise * glm::pi<float> ()) * scale) + offset;
    if (!std::isfinite (angle) || glm::dot (right, right) == 0.0f)
        return std::nullopt;
    const glm::vec3 rotated = glm::mat3 (glm::rotate (
        glm::mat4 (1.0f), angle, right)) * forward;
    return rotated * (speedRandom * speedDelta + speedMin);
}

} // namespace WallpaperEngine::Render::Utils
