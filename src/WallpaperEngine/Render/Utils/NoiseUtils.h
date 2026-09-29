#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <glm/glm.hpp>

namespace WallpaperEngine::Render::Utils {

// Perlin noise permutation table
static const unsigned char PERLIN_PERM[]
    = { 151, 160, 137, 91, 90, 15, 131, 13, 201, 95, 96, 53, 194, 233, 7, 225, 140, 36, 103, 30, 69, 142, 8, 99, 37,
	240, 21, 10, 23, 190, 6, 148, 247, 120, 234, 75, 0, 26, 197, 62, 94, 252, 219, 203, 117, 35, 11, 32, 57, 177,
	33, 88, 237, 149, 56, 87, 174, 20, 125, 136, 171, 168, 68, 175, 74, 165, 71, 134, 139, 48, 27, 166, 77, 146,
	158, 231, 83, 111, 229, 122, 60, 211, 133, 230, 220, 105, 92, 41, 55, 46, 245, 40, 244, 102, 143, 54, 65, 25,
	63, 161, 1, 216, 80, 73, 209, 76, 132, 187, 208, 89, 18, 169, 200, 196, 135, 130, 116, 188, 159, 86, 164, 100,
	109, 198, 173, 186, 3, 64, 52, 217, 226, 250, 124, 123, 5, 202, 38, 147, 118, 126, 255, 82, 85, 212, 207, 206,
	59, 227, 47, 16, 58, 17, 182, 189, 28, 42, 223, 183, 170, 213, 119, 248, 152, 2, 44, 154, 163, 70, 221, 153,
	101, 155, 167, 43, 172, 9, 129, 22, 39, 253, 19, 98, 108, 110, 79, 113, 224, 232, 178, 185, 112, 104, 218, 246,
	97, 228, 251, 34, 242, 193, 238, 210, 144, 12, 191, 179, 162, 241, 81, 51, 145, 235, 249, 14, 239, 107, 49, 192,
	214, 31, 181, 199, 106, 157, 184, 84, 204, 176, 115, 121, 50, 45, 127, 4, 150, 254, 138, 236, 205, 93, 222, 114,
	67, 29, 24, 72, 243, 141, 128, 195, 78, 66, 215, 61, 156, 180,
	// Duplicate for wrapping
	151, 160, 137, 91, 90, 15, 131, 13, 201, 95, 96, 53, 194, 233, 7, 225, 140, 36, 103, 30, 69, 142, 8, 99, 37,
	240, 21, 10, 23, 190, 6, 148, 247, 120, 234, 75, 0, 26, 197, 62, 94, 252, 219, 203, 117, 35, 11, 32, 57, 177,
	33, 88, 237, 149, 56, 87, 174, 20, 125, 136, 171, 168, 68, 175, 74, 165, 71, 134, 139, 48, 27, 166, 77, 146,
	158, 231, 83, 111, 229, 122, 60, 211, 133, 230, 220, 105, 92, 41, 55, 46, 245, 40, 244, 102, 143, 54, 65, 25,
	63, 161, 1, 216, 80, 73, 209, 76, 132, 187, 208, 89, 18, 169, 200, 196, 135, 130, 116, 188, 159, 86, 164, 100,
	109, 198, 173, 186, 3, 64, 52, 217, 226, 250, 124, 123, 5, 202, 38, 147, 118, 126, 255, 82, 85, 212, 207, 206,
	59, 227, 47, 16, 58, 17, 182, 189, 28, 42, 223, 183, 170, 213, 119, 248, 152, 2, 44, 154, 163, 70, 221, 153,
	101, 155, 167, 43, 172, 9, 129, 22, 39, 253, 19, 98, 108, 110, 79, 113, 224, 232, 178, 185, 112, 104, 218, 246,
	97, 228, 251, 34, 242, 193, 238, 210, 144, 12, 191, 179, 162, 241, 81, 51, 145, 235, 249, 14, 239, 107, 49, 192,
	214, 31, 181, 199, 106, 157, 184, 84, 204, 176, 115, 121, 50, 45, 127, 4, 150, 254, 138, 236, 205, 93, 222, 114,
	67, 29, 24, 72, 243, 141, 128, 195, 78, 66, 215, 61, 156, 180 };

// Perlin noise gradient function
inline double perlinGrad (int hash, double x, double y, double z) {
    switch (hash & 0xF) {
	case 0x0:
	    return x + y;
	case 0x1:
	    return -x + y;
	case 0x2:
	    return x - y;
	case 0x3:
	    return -x - y;
	case 0x4:
	    return x + z;
	case 0x5:
	    return -x + z;
	case 0x6:
	    return x - z;
	case 0x7:
	    return -x - z;
	case 0x8:
	    return y + z;
	case 0x9:
	    return -y + z;
	case 0xA:
	    return y - z;
	case 0xB:
	    return -y - z;
	case 0xC:
	    return y + x;
	case 0xD:
	    return -y + z;
	case 0xE:
	    return y - x;
	case 0xF:
	    return -y - z;
	default:
	    return 0;
    }
}

// Perlin noise ease curve (6t^5 - 15t^4 + 10t^3)
inline double perlinEase (double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

// Linear interpolation
inline double lerpDouble (double t, double a, double b) { return a + t * (b - a); }

// Perlin noise implementation
inline double perlinNoise (double x, double y, double z) {
    int X = static_cast<int> (std::floor (x)) & 255;
    int Y = static_cast<int> (std::floor (y)) & 255;
    int Z = static_cast<int> (std::floor (z)) & 255;

    x -= std::floor (x);
    y -= std::floor (y);
    z -= std::floor (z);

    double u = perlinEase (x);
    double v = perlinEase (y);
    double w = perlinEase (z);

    int A = PERLIN_PERM[X] + Y;
    int AA = PERLIN_PERM[A] + Z;
    int AB = PERLIN_PERM[A + 1] + Z;
    int B = PERLIN_PERM[X + 1] + Y;
    int BA = PERLIN_PERM[B] + Z;
    int BB = PERLIN_PERM[B + 1] + Z;

    return lerpDouble (
	w,
	lerpDouble (
	    v, lerpDouble (u, perlinGrad (PERLIN_PERM[AA], x, y, z), perlinGrad (PERLIN_PERM[BA], x - 1, y, z)),
	    lerpDouble (u, perlinGrad (PERLIN_PERM[AB], x, y - 1, z), perlinGrad (PERLIN_PERM[BB], x - 1, y - 1, z))
	),
	lerpDouble (
	    v,
	    lerpDouble (
		u, perlinGrad (PERLIN_PERM[AA + 1], x, y, z - 1), perlinGrad (PERLIN_PERM[BA + 1], x - 1, y, z - 1)
	    ),
	    lerpDouble (
		u, perlinGrad (PERLIN_PERM[AB + 1], x, y - 1, z - 1),
		perlinGrad (PERLIN_PERM[BB + 1], x - 1, y - 1, z - 1)
	    )
	)
    );
}

// Perlin noise vec3 (3 independent noise samples with different offsets)
inline glm::vec3 perlinNoiseVec3 (const glm::vec3& p) {
    return glm::vec3 (
	static_cast<float> (perlinNoise (p.x, p.y, p.z)),
	static_cast<float> (perlinNoise (p.x + 89.2, p.y + 33.1, p.z + 57.3)),
	static_cast<float> (perlinNoise (p.x + 100.3, p.y + 120.1, p.z + 142.2))
    );
}

// Curl noise - smooth, swirling patterns ideal for fluid-like particle motion
inline glm::vec3 curlNoise (const glm::vec3& p) {
    const float e = 1e-4f;

    glm::vec3 dx (e, 0, 0);
    glm::vec3 dy (0, e, 0);
    glm::vec3 dz (0, 0, e);

    glm::vec3 x0 = perlinNoiseVec3 (p - dx);
    glm::vec3 x1 = perlinNoiseVec3 (p + dx);
    glm::vec3 y0 = perlinNoiseVec3 (p - dy);
    glm::vec3 y1 = perlinNoiseVec3 (p + dy);
    glm::vec3 z0 = perlinNoiseVec3 (p - dz);
    glm::vec3 z1 = perlinNoiseVec3 (p + dz);

    float x = (y1.z - y0.z) - (z1.y - z0.y);
    float y = (z1.x - z0.x) - (x1.z - x0.z);
    float z = (x1.y - x0.y) - (y1.x - y0.x);

    return glm::vec3 (x, y, z) / (2.0f * e);
}

// Scalar lane of native 1400fd010, the 3-D simplex primitive called by the
// turbulence SIMD kernel. Its permutation is the same 256-entry table above;
// the native gradient-index table is permutation modulo 12. This helper is
// deliberately separate from the provisional Perlin curl operator until the
// native turbulence component permutations and field scaling are recovered.
inline std::optional<float> nativeSimplex3 (glm::vec3 point) {
    if (!std::isfinite (point.x) || !std::isfinite (point.y) || !std::isfinite (point.z)
        || std::abs (point.x) >= 1.0e6f || std::abs (point.y) >= 1.0e6f
        || std::abs (point.z) >= 1.0e6f)
        return std::nullopt;
    constexpr glm::vec3 gradients[12] = {
        {1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0},
        {1, 0, 1}, {-1, 0, 1}, {1, 0, -1}, {-1, 0, -1},
        {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1},
    };
    const float skew = (point.y + point.z + point.x) * (1.0f / 3.0f);
    const int i = static_cast<int> (std::floor (point.x + skew));
    const int j = static_cast<int> (std::floor (point.y + skew));
    const int k = static_cast<int> (std::floor (point.z + skew));
    const float unskew = static_cast<float> (i + j + k) * (1.0f / 6.0f);
    const glm::vec3 base {
        point.x - (static_cast<float> (i) - unskew),
        point.y - (static_cast<float> (j) - unskew),
        point.z - (static_cast<float> (k) - unskew),
    };
    glm::ivec3 first, second;
    if (base.x >= base.y) {
        if (base.y >= base.z) { first = {1, 0, 0}; second = {1, 1, 0}; }
        else if (base.x >= base.z) { first = {1, 0, 0}; second = {1, 0, 1}; }
        else { first = {0, 0, 1}; second = {1, 0, 1}; }
    } else {
        if (base.y < base.z) { first = {0, 0, 1}; second = {0, 1, 1}; }
        else if (base.x < base.z) { first = {0, 1, 0}; second = {0, 1, 1}; }
        else { first = {0, 1, 0}; second = {1, 1, 0}; }
    }
    const auto corner = [&] (glm::ivec3 offset, float shift, bool fourth = false) {
        const float x = (base.x - static_cast<float> (offset.x)) + shift;
        const float y = (base.y - static_cast<float> (offset.y)) + shift;
        const float z = (base.z - static_cast<float> (offset.z)) + shift;
        const float radius = ((0.6f - x * x) - y * y) - z * z;
        if (radius < 0.0f) return 0.0f;
        // The fourth corner in 1400fd010 hashes k (rather than k + 1)
        // before applying the +1 offsets to j and i. This differs from
        // conventional simplex noise and is visible in native SIMD output.
        const uint8_t zHash = PERLIN_PERM[(k + (fourth ? 0 : offset.z)) & 255];
        const uint8_t yHash = PERLIN_PERM[(zHash + j + offset.y) & 255];
        const glm::vec3 gradient = gradients[PERLIN_PERM[(yHash + i + offset.x) & 255] % 12];
        const float dot = (gradient.z * z + gradient.y * y) + gradient.x * x;
        // 1400fd5db/5e2, fd7d3 and fd965 square the radius twice before
        // multiplying the gradient dot; the decompiler's left fold differs.
        const float radiusSquared = radius * radius;
        return dot * (radiusSquared * radiusSquared);
    };
    const float value = corner ({1, 1, 1}, 0.5f, true)
        + corner (second, 1.0f / 3.0f)
        + corner (first, 1.0f / 6.0f)
        + corner ({0, 0, 0}, 0.0f);
    return value * 32.0f;
}

// Native 140242f0c–140243156 selects these cyclic coordinate orders for the
// X/Y/Z turbulence velocity streams. The caller is responsible for assembling
// the native coordinate vector and applying its speed, step and envelope.
inline std::optional<glm::vec3> nativeTurbulenceNoise (glm::vec3 coordinate, uint32_t axisMask) {
    glm::vec3 result (0.0f);
    if ((axisMask & 1u) != 0) {
        const auto x = nativeSimplex3 (coordinate);
        if (!x) return std::nullopt;
        result.x = *x;
    }
    if ((axisMask & 2u) != 0) {
        const auto y = nativeSimplex3 ({coordinate.z, coordinate.x, coordinate.y});
        if (!y) return std::nullopt;
        result.y = *y;
    }
    if ((axisMask & 4u) != 0) {
        const auto z = nativeSimplex3 ({coordinate.y, coordinate.z, coordinate.x});
        if (!z) return std::nullopt;
        result.z = *z;
    }
    return result;
}

// One finite lane of opcode 0x24 after its audio and envelope inputs have
// been resolved. The interpreter uses the same birth-random stream for phase
// and speed, and its third (damped) clock argument for the XYZ mask gains.
// This isolated helper does not choose the native scene clock or convert the
// authored particle coordinate basis; the caller must supply those explicitly.
inline std::optional<glm::vec3> nativeTurbulenceVelocityDelta (
    glm::vec3 position, float birthRandom, float phaseMin, float phaseDelta,
    float speedMin, float speedDelta, float scale, float timeScale, float sceneTime,
    glm::vec3 mask, float dampingClock, float envelope) {
    const uint32_t axisMask = (mask.x != 0.0f ? 1u : 0u)
        | (mask.y != 0.0f ? 2u : 0u) | (mask.z != 0.0f ? 4u : 0u);
    if (axisMask == 0) return glm::vec3 (0.0f);
    const float phase = (birthRandom * phaseDelta + phaseMin) + timeScale * sceneTime;
    const float speed = (birthRandom * speedDelta + speedMin) * envelope;
    const glm::vec3 coordinate = (position + glm::vec3 (phase)) * scale;
    const auto noise = nativeTurbulenceNoise (coordinate, axisMask);
    if (!noise) return std::nullopt;
    glm::vec3 delta (0.0f);
    delta.x = (noise->x * (dampingClock * mask.x)) * speed;
    delta.y = (noise->y * (dampingClock * mask.y)) * speed;
    delta.z = (noise->z * (dampingClock * mask.z)) * speed;
    return delta;
}

} // namespace WallpaperEngine::Render::Utils
