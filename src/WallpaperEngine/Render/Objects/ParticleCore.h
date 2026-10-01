#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include "WallpaperEngine/Render/Utils/NativeParticleGradientNoise.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace WallpaperEngine::Render::Objects::ParticleCore {

inline bool instanceTintDiffers (const glm::vec3& tint, const glm::vec3& preset) {
    // 14022bd40/14022f890 compare the shared tint to each node's preset RGB.
    constexpr float tolerance = 0.0035294117f;
    return std::abs (tint.x - preset.x) >= tolerance
        || std::abs (tint.y - preset.y) >= tolerance
        || std::abs (tint.z - preset.z) >= tolerance;
}

inline bool particleInstanceTintValid (
    const glm::vec3& rootTint, const glm::vec3& rootPresetTint,
    const glm::vec3& nodePresetTint, uint32_t rootFlags, uint32_t nodeFlags,
    bool rootNode) {
    const bool rootValid = rootTint.x >= 0.0f && (rootFlags & 8u) == 0
        && instanceTintDiffers (rootTint, rootPresetTint);
    return rootValid && (nodeFlags & 8u) == 0
        && (rootNode || instanceTintDiffers (rootTint, nodePresetTint));
}

inline glm::vec3 particleInstanceBirthRgbGain (
    const glm::vec3& rootTint, const glm::vec3& rootPresetTint,
    const glm::vec3& nodePresetTint, float rootBrightness,
    uint32_t rootFlags, uint32_t nodeFlags, bool tintCompiled,
    bool rootNode, bool hdrActive) {
    // The root +0xe4 tint is valid only after the root node enables its
    // color-state bit. Static/event descendants share that root state, but
    // still compare against their own preset color (14022bd40/14022f890).
    const bool nodeTintValid = particleInstanceTintValid (rootTint,
        rootPresetTint, nodePresetTint, rootFlags, nodeFlags, rootNode);
    // 1401c5490 sets compiled 0x200000 for !hascolor or scene version <5.
    // The caller supplies that compiled bit, independent of HDR quality.
    const float brightness = hdrActive && (nodeFlags & 8u) == 0
        ? rootBrightness : 1.0f;
    return glm::vec3 (brightness)
        * (nodeTintValid && tintCompiled ? rootTint : glm::vec3 (1.0f));
}

inline glm::vec3 particleRgbToHsv (const glm::vec3& rgb) {
    const float maximum = std::max ({rgb.r, rgb.g, rgb.b});
    const float minimum = std::min ({rgb.r, rgb.g, rgb.b});
    const float chroma = maximum - minimum;
    if (chroma < 1e-5f || maximum <= 0.0f)
        return {0.0f, 0.0f, maximum};
    float hue = 0.0f;
    if (rgb.r == maximum)
        hue = (rgb.g - rgb.b) / chroma;
    else if (rgb.g == maximum)
        hue = (rgb.b - rgb.r) / chroma + 2.0f;
    else
        hue = (rgb.r - rgb.g) / chroma + 4.0f;
    hue /= 6.0f;
    hue -= std::floor (hue);
    return {hue, chroma / maximum, maximum};
}

inline glm::vec3 particleHsvToRgb (const glm::vec3& hsv) {
    const float sector = (hsv.x - std::floor (hsv.x)) * 6.0f;
    const int index = static_cast<int> (std::floor (sector));
    const float fraction = sector - std::floor (sector);
    const float low = hsv.z * (1.0f - hsv.y);
    const float falling = hsv.z * (1.0f - hsv.y * fraction);
    const float rising = hsv.z * (1.0f - hsv.y * (1.0f - fraction));
    switch (index) {
    case 0: return {hsv.z, rising, low};
    case 1: return {falling, hsv.z, low};
    case 2: return {low, hsv.z, rising};
    case 3: return {low, falling, hsv.z};
    case 4: return {rising, low, hsv.z};
    default: return {hsv.z, low, falling};
    }
}

inline glm::vec3 particleInstanceShiftColorEndpoint (
    const glm::vec3& authored, const glm::vec3& rootTint,
    const glm::vec3& presetColor, bool tintValid) {
    if (!tintValid) return authored;
    // 1401d15a0 descriptor opcodes 11/12 shift the authored endpoint in HSV
    // by root-tint HSV minus compiled-preset HSV, then clamp S/V and wrap H.
    const glm::vec3 delta = particleRgbToHsv (rootTint)
        - particleRgbToHsv (presetColor);
    glm::vec3 shifted = particleRgbToHsv (authored) + delta;
    shifted.x -= std::floor (shifted.x);
    shifted.y = std::clamp (shifted.y, 0.0f, 1.0f);
    shifted.z = std::clamp (shifted.z, 0.0f, 1.0f);
    return particleHsvToRgb (shifted);
}

inline float flag4OrthographicEyeDistance (float fovDegrees, float orthoYScale) {
    // 1401e5b60: view translation is -1/(tan(fov/2)*projection[1][1]).
    // For a standard orthographic scene, projection[1][1] = 2/height.
    return 1.0f / (std::tan (glm::radians (fovDegrees) * 0.5f) * orthoYScale);
}

struct FixedRendererBasis {
    glm::vec3 right;
    glm::vec3 up;
};

inline FixedRendererBasis fixedRendererBasis (glm::vec3 axis) {
    // Factory 1401c22e0: the authored axis becomes the renderer's right
    // vector. Its up vector is Y projected off that axis, except the
    // singular +/-Y case where the factory chooses -Z.
    const float length = glm::length (axis);
    axis = length > 0.0f && std::isfinite (length)
        ? axis / length : glm::vec3 (0.0f, 0.0f, 1.0f);
    if (axis.x == 0.0f && axis.z == 0.0f)
        return {axis, glm::vec3 (0.0f, 0.0f, -1.0f)};
    const glm::vec3 up = glm::vec3 (0.0f, 1.0f, 0.0f) - axis.y * axis;
    return {axis, glm::normalize (up)};
}

inline std::optional<FixedRendererBasis> fixedRendererLocalBasis (
    glm::vec3 authoredAxis, const glm::mat3& reflectedSceneLinear, bool skipWorldTransform) {
    // 1402298b0: default flags first multiply the authored basis by the
    // scene-stack linear matrix M; flag bit 0 skips that step. Every path
    // then multiplies by M^T and normalizes right/up independently.
    const auto authored = fixedRendererBasis (authoredAxis);
    glm::vec3 right (authored.right.x, -authored.right.y, authored.right.z);
    glm::vec3 up (authored.up.x, -authored.up.y, authored.up.z);
    if (!skipWorldTransform) {
        right = reflectedSceneLinear * right;
        up = reflectedSceneLinear * up;
    }
    right = glm::transpose (reflectedSceneLinear) * right;
    up = glm::transpose (reflectedSceneLinear) * up;
    const float rightLength = glm::length (right);
    const float upLength = glm::length (up);
    if (!std::isfinite (rightLength) || !std::isfinite (upLength)
        || rightLength <= 0.0f || upLength <= 0.0f) return std::nullopt;
    return FixedRendererBasis {right / rightLength, up / upLength};
}

inline std::optional<FixedRendererBasis> uprightRendererLocalBasis (
    glm::vec3 authoredAxis, const glm::mat3& reflectedSceneLinear,
    const glm::vec3& reflectedViewRight, bool skipWorldTransform,
    const glm::vec3& sceneWorldUp = glm::vec3 (0.0f, -1.0f, 0.0f)) {
    // 1402298b0 mode 1: bit 0 chooses a fixed world Y up; otherwise the
    // factory axis is transformed by M. The two cross products remove the
    // component of view-right along that up, then M^T returns local axes.
    const auto factory = fixedRendererBasis (authoredAxis);
    const glm::vec3 reflectedAxis (
        factory.right.x, -factory.right.y, factory.right.z);
    const glm::vec3 worldUp = skipWorldTransform
        ? sceneWorldUp
        : reflectedSceneLinear * reflectedAxis;
    const glm::vec3 intermediate = glm::cross (reflectedViewRight, worldUp);
    glm::vec3 right = glm::transpose (reflectedSceneLinear)
        * glm::cross (intermediate, worldUp);
    glm::vec3 up = glm::transpose (reflectedSceneLinear) * worldUp;
    const float rightLength = glm::length (right);
    const float upLength = glm::length (up);
    if (!std::isfinite (rightLength) || !std::isfinite (upLength)
        || rightLength <= 0.0f || upLength <= 0.0f) return std::nullopt;
    return FixedRendererBasis {right / rightLength, up / upLength};
}

inline float nativeRandomUnit (std::mt19937& rng);

inline float nativeBoxAxis (float unit, float direction, float minDistance,
                            float maxDistance) {
    const float signedCoordinate = ((unit + unit) - 1.0f) * direction;
    const float sign = static_cast<float> ((signedCoordinate > 0.0f)
                                           - (signedCoordinate < 0.0f));
    const float magnitude = std::abs (signedCoordinate) * (maxDistance - minDistance)
                            + minDistance;
    return sign * magnitude;
}

inline glm::vec3 nativeBoxDisplacement (std::mt19937& rng, const glm::vec3& direction,
                                        const glm::vec3& minDistance,
                                        const glm::vec3& maxDistance) {
    // Native stores X from the third draw, Y from the second, Z from the
    // first (1402378a0:842–860); it draws even for zero ranges.
    const float z = nativeRandomUnit (rng);
    const float y = nativeRandomUnit (rng);
    const float x = nativeRandomUnit (rng);
    return {nativeBoxAxis (x, direction.x, minDistance.x, maxDistance.x),
            nativeBoxAxis (y, direction.y, minDistance.y, maxDistance.y),
            nativeBoxAxis (z, direction.z, minDistance.z, maxDistance.z)};
}

inline bool nativeBoxNeedsFallback (const glm::vec3& displacement) {
    // Opcode 2 uses one Quake-style inverse-sqrt step, then compares its
    // reciprocal to 1e-4 (1402378a0:978–992 / 1402389cd–a3a).
    const float y2 = displacement.y * displacement.y;
    const float x2 = displacement.x * displacement.x;
    const float z2 = displacement.z * displacement.z;
    const float squaredLength = (y2 + x2) + z2;
    float estimate = std::bit_cast<float> (
        0x5f375a86u - (std::bit_cast<uint32_t> (squaredLength) >> 1));
    float correction = squaredLength * 0.5f;
    correction *= estimate;
    correction *= estimate;
    correction = 1.5f - correction;
    estimate *= correction;
    return 1.0f / estimate < 0.0001f;
}

inline glm::vec3 nativeSphereDisplacement (float azimuthUnit, float axialUnit,
                                           float radiusUnit, const glm::vec3& directions,
                                           float distanceMin, float distanceMax,
                                           float cone, const glm::ivec3& sign) {
    // Opcode 1, 1402378a0:527–600: angle, axial cone coordinate and cube-root
    // radius are independent draws. The vector is normalized *after* the
    // authored direction scaling; its pre-normalized length remaps distance.
    const float azimuth = azimuthUnit * glm::two_pi<float> ();
    const float coneStart = -std::cos (cone * glm::pi<float> ());
    const float axial = axialUnit * (1.0f - coneStart) + coneStart;
    const float radius = std::cbrt (radiusUnit);
    const float equator = std::sqrt (std::max (0.0f, 1.0f - axial * axial));
    glm::vec3 sample (radius * axial * directions.x,
                      std::sin (azimuth) * radius * equator * directions.y,
                      std::cos (azimuth) * radius * equator * directions.z);
    const float length = glm::length (sample);
    if (length == 0.0f) return glm::vec3 (0.0f);
    sample /= length;
    for (int axis = 0; axis < 3; ++axis) {
        if (sign[axis] > 0) sample[axis] = std::abs (sample[axis]);
        else if (sign[axis] < 0) sample[axis] = -std::abs (sample[axis]);
    }
    return sample * (distanceMin + (distanceMax - distanceMin) * length);
}

inline glm::vec2 emitterSpeedBounds (float authoredMin, float authoredMax,
                                     float instanceSpeed, uint32_t particleFlags) {
    // Both box and sphere factories bind their velocity range to the shared
    // instance speed descriptor unless preset bit 0x10 suppresses the patch.
    const float factor = (particleFlags & 0x10u) != 0u ? 1.0f : instanceSpeed;
    return { authoredMin * factor, authoredMax * factor };
}

struct ImageEmitterSample {
    uint8_t red, green, blue;
    uint8_t bone = 0xff;
    int16_t x, y;
};

inline glm::uvec2 imageEmitterReadbackSize (uint32_t sourceWidth, uint32_t sourceHeight) {
    // 1401d3ae0 caps the source at 3840x2160 while preserving its aspect,
    // then divides each capped dimension by four with a two-pixel floor.
    if (sourceWidth == 0 || sourceHeight == 0) return {};
    uint32_t boundedWidth = sourceWidth, boundedHeight = sourceHeight;
    if (sourceWidth > 3840 || sourceHeight > 2160) {
        const float aspect = static_cast<float> (sourceWidth)
            / static_cast<float> (sourceHeight);
        if (aspect < 3840.0f / 2160.0f) {
            boundedHeight = 2160;
            boundedWidth = static_cast<uint32_t> (aspect * 2160.0f);
        } else {
            boundedWidth = 3840;
            boundedHeight = static_cast<uint32_t> (3840.0f / aspect);
        }
    }
    return {std::max (2u, boundedWidth / 4), std::max (2u, boundedHeight / 4)};
}

inline std::vector<ImageEmitterSample> imageEmitterSamples (
    const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height,
    uint32_t sourceWidth, uint32_t sourceHeight) {
    // 1401d3ae0: alpha > 0x7e, X outer/Y inner, centered signed 16-bit
    // coordinates. The source-width ratio is used for both axes.
    std::vector<ImageEmitterSample> result;
    if (width == 0 || height == 0 || sourceWidth == 0 || sourceHeight == 0
        || width > rgba.size () / 4 / height) return result;
    const float scale = static_cast<float> (sourceWidth) / static_cast<float> (width);
    const float halfStep = scale * 0.5f;
    for (uint32_t x = 0; x < width; ++x) {
        for (uint32_t y = 0; y < height; ++y) {
            const size_t offset = (static_cast<size_t> (y) * width + x) * 4;
            if (rgba[offset + 3] <= 0x7e) continue;
            const int32_t sx = static_cast<int32_t> (
                scale * static_cast<float> (x) - static_cast<float> (sourceWidth >> 1) + halfStep);
            const int32_t sy = static_cast<int32_t> (
                scale * static_cast<float> (y) - static_cast<float> (sourceHeight >> 1) + halfStep);
            result.push_back ({rgba[offset], rgba[offset + 1], rgba[offset + 2], 0xff,
                              static_cast<int16_t> (sx), static_cast<int16_t> (sy)});
        }
    }
    return result;
}

inline uint32_t nativeImageSampleIndex (std::mt19937& rng, uint32_t count) {
    // 1402378a0:1163–1191: mt19937 integer draw with multiply-high
    // rejection. A one-entry list still consumes one word.
    if (count == 0) return 0;
    const uint32_t threshold = static_cast<uint32_t> (-count) % count;
    for (;;) {
        const uint64_t product = static_cast<uint64_t> (rng ()) * count;
        if (static_cast<uint32_t> (product) >= threshold)
            return static_cast<uint32_t> (product >> 32);
    }
}

inline glm::mat3 localControlPointBasis (const glm::vec3& authoredAngles) {
    // Native 14022bd40:114–133 writes Rz * Ry * Rx from radian XYZ angles.
    // Particle simulation reflects authored Y, so conjugate by that flip.
    const float cx = std::cos (authoredAngles.x), sx = std::sin (authoredAngles.x);
    const float cy = std::cos (authoredAngles.y), sy = std::sin (authoredAngles.y);
    const float cz = std::cos (authoredAngles.z), sz = std::sin (authoredAngles.z);
    const glm::mat3 authored (
        glm::vec3 (cy * cz, cy * sz, -sy),
        glm::vec3 (sy * cz * sx - cx * sz, sy * sz * sx + cx * cz, cy * sx),
        glm::vec3 (cx * sy * cz + sx * sz, cx * sy * sz - sx * cz, cx * cy));
    glm::mat3 flip (1.0f);
    flip[1][1] = -1.0f;
    return flip * authored * flip;
}

// Native emitter allocation scans the SoA live-marker stream from slot zero
// each tick and raises the high-water mark only when choosing its end slot.
class NativeSlotAllocation {
public:
    std::optional<uint32_t> allocate (uint32_t capacity) {
        if (!m_free.empty ()) {
            const uint32_t slot = *m_free.begin ();
            if (slot >= capacity) return std::nullopt;
            m_free.erase (m_free.begin ());
            return slot;
        }
        if (m_highWater >= capacity) return std::nullopt;
        return m_highWater++;
    }

    void release (uint32_t slot) {
        if (slot < m_highWater) m_free.insert (slot);
    }

    [[nodiscard]] uint32_t highWater () const { return m_highWater; }

private:
    uint32_t m_highWater { 0 };
    std::set<uint32_t> m_free;
};

// Both vortex non-ring opcodes clamp distance before interpolating their
// packed inner speed and outer-minus-inner speed. Equal endpoints use fallback
// reciprocal 1.0, rather than a divide-by-zero or zero reciprocal.
inline float vortexRadialSpeed (float distance, float innerDistance, float outerDistance,
                                float innerSpeed, float outerSpeed) {
    const float span = outerDistance - innerDistance;
    const float reciprocal = span == 0.0f ? 1.0f : 1.0f / span;
    const float t = std::clamp ((distance - innerDistance) * reciprocal, 0.0f, 1.0f);
    return innerSpeed + (outerSpeed - innerSpeed) * t;
}

// Native opcode 1 consumes two independently supplied operator arguments.
struct MovementTime {
    float integration;
    float damping;

    // Other operators currently consume the integration clock only.
    operator float () const { return integration; }
};

inline float vortexRadialVelocityScale (float distance, float innerDistance, float outerDistance,
                                        float innerSpeed, float outerSpeed, MovementTime time) {
    return vortexRadialSpeed (distance, innerDistance, outerDistance, innerSpeed, outerSpeed)
        * time.damping;
}

// Native v2 non-ring case predicts the next radial position with the
// integration clock, then applies a velocity correction that maintains the
// current radius. Center force zero or a stationary particle yields no pull.
inline glm::vec3 vortexV2RadialCorrection (glm::vec3 currentRadial, glm::vec3 predictedRadial,
                                           float centerForce, MovementTime time) {
    if (centerForce == 0.0f || time.integration <= 0.0f) return {};
    const float currentRadius = glm::length (currentRadial);
    const float predictedRadius = glm::length (predictedRadial);
    if (!std::isfinite (currentRadius) || !std::isfinite (predictedRadius)
        || predictedRadius <= 0.0f) return {};
    return predictedRadial * ((currentRadius / predictedRadius - 1.0f)
        * centerForce / time.integration);
}

inline glm::vec3 vortexCenter (glm::vec3 controlPointPosition,
                               glm::vec3 authoredOffset, bool v2) {
    return controlPointPosition + (v2 ? glm::vec3 (0.0f) : authoredOffset);
}

inline glm::vec3 vortexAxis (glm::vec3 authoredAxis, glm::mat3 controlPointBasis, bool v2) {
    return v2 ? controlPointBasis * authoredAxis : authoredAxis;
}

inline glm::vec3 normalizedVortexAxis (glm::vec3 authoredAxis) {
    const float lengthSquared = glm::dot (authoredAxis, authoredAxis);
    return lengthSquared >= 0.001f && std::isfinite (lengthSquared)
        ? authoredAxis / std::sqrt (lengthSquared) : glm::vec3 (1.0f, 0.0f, 0.0f);
}

// The native radial cross axis term changes handedness under the renderer's
// Y reflection. Both radial and authored axis must first enter simulation space.
inline glm::vec3 toSimulationVector (glm::vec3 authored) {
    authored.y = -authored.y;
    return authored;
}

inline glm::vec3 toAuthoredVector (glm::vec3 simulation) {
    return toSimulationVector (simulation); // Y reflection is its own inverse
}

// Native local control-point translation is authored alongside emitter origin.
// The Linux particle streams use the reflected simulation basis.
inline glm::vec3 localControlPointPosition (glm::vec3 authoredOffset) {
    return toSimulationVector (authoredOffset);
}

// Native operator 0x0a uses a linear radial envelope inside the full authored
// threshold. Its default flag bit 1 limits an inward impulse to the remaining
// distance, avoiding a one-step overshoot at the control point.
inline glm::vec3 controlPointAttractVelocityDelta (glm::vec3 particle,
                                                   glm::vec3 center, float scale,
                                                   float threshold, MovementTime time,
                                                   uint32_t flags) {
    const glm::vec3 radial = particle - center;
    const float distance = glm::length (radial);
    if (!(distance > 0.0f && distance < threshold) || !std::isfinite (distance)
        || !std::isfinite (scale) || !std::isfinite (threshold)
        || !std::isfinite (time.damping))
        return {};
    float impulse = (1.0f - distance / threshold) * scale * time.damping;
    if ((flags & 2u) != 0u && distance < impulse) impulse = distance;
    return -radial * (impulse / distance);
}

inline glm::vec3 instanceControlPointPosition (glm::vec3 authoredOffset,
                                                glm::vec3 authoredOverride, uint32_t flags) {
    const bool allowed = (flags & 0x10005u) == 0;
    const bool present = authoredOverride.x != std::numeric_limits<float>::max ();
    return localControlPointPosition (allowed && present ? authoredOverride : authoredOffset);
}

// Native random-frame mode forwards the particle's birth random stream to
// in_ParticleLifeTime. genericparticle.vert applies frac before atlas lookup.
inline float randomFrameLifetime (float birthRandom) {
    return birthRandom - std::floor (birthRandom);
}

inline glm::vec3 vortexTangent (glm::vec3 radial, glm::vec3 axis) {
    const glm::vec3 tangent = glm::cross (axis, radial);
    const float length = glm::length (tangent);
    return length > 0.001f && std::isfinite (length)
        ? tangent / length : glm::vec3 (0.0f);
}

inline int vortexControlPointIndex (int authoredIndex) {
    return static_cast<int> (std::min (static_cast<uint32_t> (authoredIndex), 7u));
}

struct VortexRingInfluence {
    float speedInterpolation;
    float signedPull;
};

inline VortexRingInfluence vortexV2RingInfluence (float distance, float radius,
                                                  float width, float pullDistance) {
    const float gap = std::abs (radius - distance) - width;
    const float reciprocal = pullDistance == 0.0f ? 1.0f : 1.0f / pullDistance;
    const float t = std::clamp (gap * reciprocal, 0.0f, 1.0f);
    // Native bit selection suppresses pull at t=0, even though 1-t is one.
    const float pull = t > 0.0f && t < 1.0f
        ? std::copysign (1.0f - t, radius - distance) : 0.0f;
    return { t, pull };
}

inline float dampingTime (float frameDuration, float operatorDuration) {
    if (frameDuration <= 0.0f) {
        return operatorDuration;
    }
    return std::pow (std::min (1.0f, 0.025f / frameDuration), 0.7f) * operatorDuration;
}

// The scene clock supplies the damping ratio, while the particle node supplies
// the duration of age/emission and operator work. Native FPS values 1..20 run
// two half-duration operator passes after one age/emission pass.
struct TickClock {
    float nodeDuration { 0.0f };
    MovementTime operatorTime { 0.0f, 0.0f };
    uint32_t operatorPasses { 0 };
};

inline TickClock tickClock (float sceneDuration, float nodeDuration, uint32_t configuredFps) {
    if (!std::isfinite (sceneDuration) || !std::isfinite (nodeDuration)
        || sceneDuration <= 0.0f || nodeDuration <= 0.0f) return {};
    const float q = dampingTime (sceneDuration, nodeDuration);
    if (configuredFps >= 1 && configuredFps <= 20) {
        return { nodeDuration, { nodeDuration * 0.5f, q * 0.5f }, 2 };
    }
    return { nodeDuration, { nodeDuration, q }, 1 };
}

// Native pre-simulation calls the inner tick directly while the outer scene
// duration is still zero. Its damping ratio therefore saturates to one.
inline TickClock warmupClock (float step, uint32_t configuredFps) {
    if (!std::isfinite (step) || step <= 0.0f) return {};
    if (configuredFps >= 1 && configuredFps <= 20) {
        return { step, { step * 0.5f, step * 0.5f }, 2 };
    }
    return { step, { step, step }, 1 };
}

struct WarmupPlan {
    float step { 0.0f };
    uint32_t steps { 0 };
    bool truncated { false };
};

// Native startup uses full 0.05/0.2-second steps and may overshoot starttime;
// it does not run a shortened final remainder. Bound Linux startup work for
// malformed or extreme authored durations.
constexpr uint32_t MAX_WARMUP_STEPS = 1000;

inline WarmupPlan warmupPlan (float startTime, uint32_t capacity) {
    if (!std::isfinite (startTime) || startTime <= 0.0f) return {};
    const float step = capacity < 500 ? 0.05f : 0.2f;
    float elapsed = 0.0f;
    uint32_t steps = 0;
    while (elapsed < startTime && steps < MAX_WARMUP_STEPS) {
        elapsed += step;
        ++steps;
    }
    return { step, steps, elapsed < startTime };
}

template <typename AgeAndEmit, typename BeforeOperators, typename Operators>
void dispatchTick (TickClock clock, AgeAndEmit&& ageAndEmit,
                   BeforeOperators&& beforeOperators, Operators&& operators) {
    if (clock.operatorPasses == 0) return;
    ageAndEmit (clock.nodeDuration);
    for (uint32_t pass = 0; pass < clock.operatorPasses; ++pass) {
        beforeOperators ();
        operators (clock.operatorTime);
    }
}

template <typename AgeAndEmit, typename Operators>
void dispatchTick (TickClock clock, AgeAndEmit&& ageAndEmit, Operators&& operators) {
    dispatchTick (clock, std::forward<AgeAndEmit> (ageAndEmit), [] {},
                  std::forward<Operators> (operators));
}

inline void integrateAxis (float& position, float& velocity, float packedGravity, float drag,
                           MovementTime time) {
    velocity = velocity + packedGravity * time.integration;
    position = position + velocity * time.integration;
    // The native opcode caps drag*q below one, preserving a small positive factor.
    constexpr float maxDragStep = 0x1.fffffcp-1f; // 0x3f7ffffe
    const float dragStep = std::min (drag * time.damping, maxDragStep);
    velocity = velocity * (1.0f - dragStep);
}

// Native initializer opcodes 0x0a/0x0c add each sampled component. The opcode
// itself does not apply instance speed; upstream record packing remains under
// separate review. Repeated initializer records compose.
inline void addAngularSample (float& stream, float sample) {
    if (!std::isfinite (stream) || !std::isfinite (sample)) return;
    const float next = stream + sample;
    if (std::isfinite (next)) stream = next;
}

// Native angularmovement opcode 2 updates velocity before orientation, and
// uses the same two clocks as movement opcode 1. Reject nonfinite authored
// values on Linux so an invalid override cannot poison later transforms.
inline void integrateAngularAxis (float& rotation, float& velocity, float force, float drag,
                                  MovementTime time) {
    if (!std::isfinite (rotation) || !std::isfinite (velocity)
        || !std::isfinite (force) || !std::isfinite (drag)
        || !std::isfinite (time.integration) || !std::isfinite (time.damping)) return;
    const float accelerated = velocity + force * time.integration;
    const float nextRotation = rotation + accelerated * time.integration;
    constexpr float maxDragStep = 0x1.fffffcp-1f;
    const float dragStep = std::min (drag * time.damping, maxDragStep);
    const float nextVelocity = accelerated * (1.0f - dragStep);
    if (std::isfinite (nextRotation) && std::isfinite (nextVelocity)) {
        rotation = nextRotation;
        velocity = nextVelocity;
    }
}

struct BlendEnvelope {
    float inStart { 0.0f };
    float inEnd { 0.0f };
    float outStart { 1.0f };
    float outEnd { 1.0f };
};

// The native builder only switches an operator to its envelope opcode when
// this predicate is true, after separating coincident endpoints.
inline bool usesBlendOpcode (BlendEnvelope envelope) {
    if (!std::isfinite (envelope.inStart) || !std::isfinite (envelope.inEnd)
        || !std::isfinite (envelope.outStart) || !std::isfinite (envelope.outEnd)) return false;
    const float inStart = std::min (envelope.inStart, envelope.inEnd - 0.0001f);
    const float outEnd = std::max (envelope.outEnd, envelope.outStart + 0.0001f);
    return (0.01f < envelope.inEnd || envelope.outStart < 0.99f)
        && (0.01f < envelope.outStart - envelope.inEnd
            || 0.01f < envelope.inEnd - inStart
            || 0.01f < outEnd - envelope.outStart);
}

// Native 1401c2a40 packs adjusted endpoints and reciprocal durations;
// 14022a530 multiplies the two saturated ramps at normalized lifetime age.
inline float blendWeight (float age, BlendEnvelope envelope) {
    if (!std::isfinite (age) || !std::isfinite (envelope.inStart)
        || !std::isfinite (envelope.inEnd) || !std::isfinite (envelope.outStart)
        || !std::isfinite (envelope.outEnd)) return 0.0f;
    const float inStart = std::min (envelope.inStart, envelope.inEnd - 0.0001f);
    const float outEnd = std::max (envelope.outEnd, envelope.outStart + 0.0001f);
    const float in = std::clamp ((age - inStart) / (envelope.inEnd - inStart), 0.0f, 1.0f);
    const float out = std::clamp ((outEnd - age) / (outEnd - envelope.outStart), 0.0f, 1.0f);
    return in * out;
}

inline float blendedVelocityDelta (float delta, float normalizedAge,
                                   std::optional<BlendEnvelope> envelope) {
    if (!envelope || !usesBlendOpcode (*envelope)) return delta;
    return delta * blendWeight (normalizedAge, *envelope);
}

inline float blendedMultiplier (float multiplier, float normalizedAge,
                                std::optional<BlendEnvelope> envelope) {
    if (!envelope || !usesBlendOpcode (*envelope)) return multiplier;
    return 1.0f + (multiplier - 1.0f) * blendWeight (normalizedAge, *envelope);
}

// Native capvelocity opcodes 0x12/0x26 use one length across the three
// velocity streams, then scale each component by the same factor. The
// envelope interpolates only the excess-speed reduction toward no change.
inline float capVelocityFactor (float x, float y, float z, float maxSpeed,
                                float normalizedAge,
                                std::optional<BlendEnvelope> envelope) {
    if (!std::isfinite (x) || !std::isfinite (y) || !std::isfinite (z)
        || !std::isfinite (maxSpeed)) return 1.0f;
    const float length = std::sqrt (x * x + y * y + z * z);
    if (!std::isfinite (length) || length <= 0.0f) return 1.0f;
    const float capped = std::min (1.0f, maxSpeed / length);
    return blendedMultiplier (capped, normalizedAge, envelope);
}

// Scene loader 140186c90 propagates its orthographic selection to the
// context flag read by particle default constructors. Use the parser's
// authoritative result so signed dimensions cannot select divergent modes.
inline float capVelocityDefault (bool isOrthogonal) {
    return isOrthogonal ? 100.0f : 1.0f;
}

// 1401c5490 passes the same scene-context bit to 1401bb030. An omitted
// turbulent speed bound is inserted as 100/250 in orthographic scenes and
// 0.5/1 in perspective scenes; authored bounds remain independent.
inline glm::vec2 turbulentVelocitySpeedDefaults (bool isOrthogonal) {
    return isOrthogonal
        ? glm::vec2 (100.0f, 250.0f) : glm::vec2 (0.5f, 1.0f);
}

struct VortexDefaults {
    float distanceInner;
    float distanceOuter;
    float speedInner;
};

inline VortexDefaults vortexDefaults (bool isOrthogonal) {
    return isOrthogonal
        ? VortexDefaults { 500.0f, 650.0f, 2500.0f }
        : VortexDefaults { 1.0f, 2.0f, 1.0f };
}

enum class RemapTransform { Identity, Sine, Square, Saw, Triangle, SimplexNoise, FBMNoise };

// Scalar equivalent of the native fallback 2-D simplex noise method at
// 1400fb820. Each lane hashes its birth random's float bits into the lattice.
inline float remapSimplexNoise (uint32_t seed, float coordinate) {
    if (!std::isfinite (coordinate) || std::abs (coordinate) > 1.0e8f)
        return 0.0f;
    constexpr uint32_t xPrime = 0x1dde90c9u, yPrime = 0x43c42e4du;
    constexpr uint32_t hashPrime = 0x27d4eb2du;
    const float x = coordinate;
    const float y = 0.0f;
    const float skew = (x + y) * 0.3660254f;
    const float ixFloat = std::floor (x + skew);
    const float iyFloat = std::floor (y + skew);
    const int32_t ix = static_cast<int32_t> (ixFloat);
    const int32_t iy = static_cast<int32_t> (iyFloat);
    const float unskew = (ixFloat + iyFloat) * 0.21132487f;
    const float x0 = x - (ixFloat - unskew);
    const float y0 = y - (iyFloat - unskew);
    const bool xFirst = y0 < x0;
    const float x1 = x0 - (xFirst ? 1.0f : 0.0f) + 0.21132487f;
    const float y1 = y0 - (xFirst ? 0.0f : 1.0f) + 0.21132487f;
    const float x2 = x0 - 0.57735026f;
    const float y2 = y0 - 0.57735026f;
    const uint32_t hx = static_cast<uint32_t> (ix) * xPrime;
    const uint32_t hy = static_cast<uint32_t> (iy) * yPrime;
    const auto hash = [] (uint32_t value) {
        value *= hashPrime;
        return value ^ static_cast<uint32_t> (std::bit_cast<int32_t> (value) >> 15);
    };
    const auto corner = [&] (float x, float y, uint32_t latticeHash) {
        const float radius = std::max (0.5f - x * x - y * y, 0.0f);
        const uint32_t h = hash (latticeHash);
        const float signedX = (h & 1u) != 0 ? -x : x;
        const float signedY = (h & 2u) != 0 ? -y : y;
        const float gradient = (h & 4u) != 0
            ? signedY * 2.4142137f + signedX
            : signedX * 2.4142137f + signedY;
        const float square = radius * radius;
        return square * square * gradient;
    };
    return (corner (x0, y0, hx ^ hy ^ seed)
          + corner (x1, y1, (hx + (xFirst ? xPrime : 0u))
                           ^ (hy + (xFirst ? 0u : yPrime)) ^ seed)
          + corner (x2, y2, (hx + xPrime) ^ (hy + yPrime) ^ seed)) * 38.283688f;
}

inline float remapFBMNoise (uint32_t seedBits, float coordinate, int octaves) {
    const int count = std::clamp (octaves, 1, 32);
    float geometricSum = 1.0f;
    float geometricTerm = 0.5f;
    for (int octave = 1; octave < count; ++octave) {
        geometricSum += geometricTerm;
        geometricTerm *= 0.5f;
    }
    float amplitude = 1.0f / geometricSum;
    float total = remapSimplexNoise (seedBits, coordinate) * amplitude;
    for (int octave = 1; octave < count; ++octave) {
        const float previousSeed = std::bit_cast<float> (seedBits);
        const int32_t truncated = std::isfinite (previousSeed)
            && previousSeed >= -2147483648.0f && previousSeed < 2147483648.0f
            ? static_cast<int32_t> (previousSeed) : std::numeric_limits<int32_t>::min ();
        seedBits = std::bit_cast<uint32_t> (static_cast<float> (
            std::bit_cast<int32_t> (static_cast<uint32_t> (truncated) + 1u)));
        coordinate *= 2.0f;
        amplitude *= 0.5f;
        total += remapSimplexNoise (seedBits, coordinate) * amplitude;
    }
    return total;
}

struct ScalarRemapRange {
    float inputMin { 0.0f };
    float inputMax { 1.0f };
    float outputMin { 0.0f };
    float outputMax { 1.0f };
    int flags { 1 };
    RemapTransform transform { RemapTransform::Identity };
    float transformScale { 2.0f };
    uint32_t noiseSeedBits { 0u };
    int noiseOctaves { 3 };
};

enum class RemapOperation { Set, Multiply, Add, Subtract };
enum class RemapVectorComponent { All, X, Y, Z, Sum, Average, Max, Min };

inline float remapBirthControlPointClamp01 (float value) {
    // 1401d8360 MAXSS/MINSS use value as the second operand, retaining NaN
    // and signed zero. Its decompiled conditional NaN-to-zero is inaccurate.
    return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

inline float reduceRemapVector (float x, float y, float z,
                                RemapVectorComponent component) {
    switch (component) {
    case RemapVectorComponent::All: return x; // scalar output consumes first native lane
    case RemapVectorComponent::X: return x;
    case RemapVectorComponent::Y: return y;
    case RemapVectorComponent::Z: return z;
    case RemapVectorComponent::Sum: return (x + y) + z;
    case RemapVectorComponent::Average: return ((x + y) + z) * 0.33333334f;
    case RemapVectorComponent::Max: return std::max (std::max (x, y), z);
    case RemapVectorComponent::Min: return std::min (std::min (x, y), z);
    }
    return x;
}

inline float reduceBirthRemapVector (float x, float y, float z,
                                            RemapVectorComponent component) {
    // 14023d605..63f reduces Y/Z first, then X; MAXSS/MINSS helpers
    // choose the first argument for equal/unordered operands.
    if (component == RemapVectorComponent::Max) return std::max (x, std::max (y, z));
    if (component == RemapVectorComponent::Min) return std::min (x, std::min (y, z));
    return reduceRemapVector (x, y, z, component);
}

inline float remapVectorInput (glm::vec3 value, RemapVectorComponent component,
                               bool simulationCoordinates) {
    if (simulationCoordinates) value = toAuthoredVector (value);
    return reduceRemapVector (value.x, value.y, value.z, component);
}

inline float remapControlPointDistance (glm::vec3 particlePosition,
                                       glm::vec3 controlPointPosition) {
    return glm::length (particlePosition - controlPointPosition);
}

inline float remapPositionBetweenControlPoints (glm::vec3 particlePosition,
                                               glm::vec3 first, glm::vec3 second) {
    const glm::vec3 axis = second - first;
    const float squaredLength = glm::dot (axis, axis);
    return squaredLength > 0.0f && std::isfinite (squaredLength)
        ? glm::dot (particlePosition - first, axis) / squaredLength : 0.0f;
}

enum class RemapControlPointVector { Position, Delta, Direction };

inline glm::vec3 remapControlPointVector (glm::vec3 particlePosition,
                                         glm::vec3 controlPointPosition,
                                         RemapControlPointVector selector) {
    if (selector == RemapControlPointVector::Position)
        return toAuthoredVector (controlPointPosition);
    const glm::vec3 delta = controlPointPosition - particlePosition;
    if (selector == RemapControlPointVector::Delta)
        return toAuthoredVector (delta);
    const float squaredLength = glm::dot (delta, delta);
    return squaredLength > 0.0f && std::isfinite (squaredLength)
        ? toAuthoredVector (delta * glm::inversesqrt (squaredLength))
        : glm::vec3 (0.0f);
}

// Native remap selector 6 zeros angular-speed input unless node flag bit 2
// is set. Factory sets that bit for rotationrandom and angularmovement.
inline float gatedAngularSpeed (float angularSpeedZ, bool hasRotationRandom,
                                bool hasAngularMovement) {
    return hasRotationRandom || hasAngularMovement ? angularSpeedZ : 0.0f;
}

// Remapvalue opcodes 0x13/0x27 with scalar input/output and native
// set/multiply/add/subtract operations and the recovered sine transform.
// The envelope clock is independent
// of the selected input stream and its range/clamp.
inline float remapScalarValue (float currentValue, float inputValue,
                               float normalizedAge, RemapOperation operation,
                               std::optional<BlendEnvelope> envelope = std::nullopt,
                               ScalarRemapRange range = {},
                               bool birthControlPointIEEE = false, bool birthNoise = false) {
    // Birth CP inputs and birth noise retain native unchecked IEEE results.
    const bool rawIEEE = birthControlPointIEEE || birthNoise;
    if (!rawIEEE && (!std::isfinite (currentValue) || !std::isfinite (inputValue)
        || !std::isfinite (normalizedAge)
        || !std::isfinite (range.inputMin) || !std::isfinite (range.inputMax)
        || !std::isfinite (range.outputMin) || !std::isfinite (range.outputMax)
        || !std::isfinite (range.transformScale)))
        return currentValue;
    const float span = range.inputMax - range.inputMin;
    const float safeSpan = span == 0.0f ? 0x1p-23f : span;
    float normalized = (inputValue - range.inputMin) / safeSpan;
    if ((range.flags & 1) != 0) normalized = rawIEEE
        ? remapBirthControlPointClamp01 (normalized) : std::clamp (normalized, 0.0f, 1.0f);
    if (range.transform == RemapTransform::Sine) {
        const float packedScale = range.transformScale * 3.1415927f;
        normalized = (std::sin (normalized * packedScale - 1.5707964f) + 1.0f) * 0.5f;
    } else if (range.transform == RemapTransform::Square) {
        const float scaled = normalized * range.transformScale;
        const float fraction = scaled - std::trunc (scaled);
        // Native ROUNDPS immediate 8 rounds the fraction to nearest-even,
        // independent of the process floating-point rounding mode.
        normalized = rawIEEE && !std::isfinite (fraction) ? fraction
            : (fraction > 0.5f ? 1.0f : fraction < -0.5f ? -1.0f : 0.0f)
                + (scaled < 0.0f ? 1.0f : 0.0f);
    } else if (range.transform == RemapTransform::Saw) {
        const float scaled = normalized * range.transformScale;
        normalized = scaled - std::trunc (scaled)
                   + (normalized < 0.0f ? 1.0f : 0.0f);
    } else if (range.transform == RemapTransform::Triangle) {
        const float scaled = std::abs (normalized * range.transformScale);
        normalized = 1.0f - std::abs ((scaled - std::trunc (scaled)) * 2.0f - 1.0f);
    } else if (range.transform == RemapTransform::SimplexNoise) {
        normalized = (birthNoise
            ? Utils::nativeBirthParticleGradientNoise (normalized * range.transformScale)
            : remapSimplexNoise (range.noiseSeedBits, normalized * range.transformScale)) * 0.5f + 0.5f;
    } else if (range.transform == RemapTransform::FBMNoise) {
        normalized = (birthNoise
            ? Utils::nativeBirthParticleFBMNoise (normalized, range.transformScale, range.noiseOctaves)
            : remapFBMNoise (range.noiseSeedBits, normalized * range.transformScale, range.noiseOctaves)) * 0.5f + 0.5f;
    }
    float mapped = range.outputMin + normalized * (range.outputMax - range.outputMin);
    if ((range.flags & 2) != 0) mapped = rawIEEE
        ? remapBirthControlPointClamp01 (mapped) : std::clamp (mapped, 0.0f, 1.0f);
    float target = currentValue;
    switch (operation) {
    case RemapOperation::Set: target = mapped; break;
    case RemapOperation::Multiply: target = currentValue * mapped; break;
    case RemapOperation::Add: target = currentValue + mapped; break;
    case RemapOperation::Subtract: target = currentValue - mapped; break;
    }
    if (!envelope || !usesBlendOpcode (*envelope)) return target;
    return currentValue + (target - currentValue) * blendWeight (normalizedAge, *envelope);
}

inline float remapScalarMultiply (float currentValue, float inputValue,
                                  float normalizedAge,
                                  std::optional<BlendEnvelope> envelope = std::nullopt,
                                  ScalarRemapRange range = {}) {
    return remapScalarValue (currentValue, inputValue, normalizedAge,
                             RemapOperation::Multiply, envelope, range);
}

// Birth output bit 2 is allocated by angularvelocityrandom or angularmovement;
// rotationrandom alone allocates only bit 1 (native 1401c5490).
inline float remapBirthAngularSpeed (float current, float input, RemapOperation operation,
                                    ScalarRemapRange range, bool angularVelocityRandom,
                                    bool angularMovement, bool birthControlPointIEEE = false, bool birthNoise = false) {
    if (!(angularVelocityRandom || angularMovement)) return current;
    return remapScalarValue (current, input, 0.0f, operation, std::nullopt, range,
                             birthControlPointIEEE, birthNoise);
}

inline glm::vec3 remapSpeedOutput (glm::vec3 velocity, float mappedSpeed,
                                  bool birthControlPointIEEE = false) {
    const float currentSpeed = glm::length (velocity);
    // Birth output 4 divides only for nonzero current speed; both branches
    // multiply every component, retaining 0*NaN and other IEEE outcomes.
    if (birthControlPointIEEE)
        return velocity * (currentSpeed != 0.0f ? mappedSpeed / currentSpeed : mappedSpeed);
    return currentSpeed > 0.0f && std::isfinite (currentSpeed)
        && std::isfinite (mappedSpeed)
        ? velocity * (mappedSpeed / currentSpeed) : velocity;
}

inline glm::vec3 remapVectorValue (glm::vec3 current, glm::vec3 input,
                                    float normalizedAge, RemapOperation operation,
                                    glm::vec3 inputMin, glm::vec3 inputMax,
                                    glm::vec3 outputMin, glm::vec3 outputMax,
                                    int flags, std::optional<BlendEnvelope> envelope,
                                    int outputComponent,
                                    RemapTransform transform = RemapTransform::Identity,
                                    float transformScale = 2.0f,
                                    uint32_t noiseSeedBits = 0u,
                                    bool vectorNoiseInput = false,
                                    int noiseOctaves = 3,
                                    bool birthControlPointIEEE = false, bool birthNoise = false) {
    for (int axis = 0; axis < 3; ++axis) {
        if (outputComponent != 0 && outputComponent != axis + 1) continue;
        uint32_t axisSeed = noiseSeedBits;
        if ((transform == RemapTransform::SimplexNoise || transform == RemapTransform::FBMNoise)
            && vectorNoiseInput) {
            const uint32_t salt = axis == 1 ? 0x0b3924adu
                                : axis == 2 ? 0x493a8e83u : 0u;
            axisSeed ^= salt;
        }
        current[axis] = remapScalarValue (current[axis], input[axis], normalizedAge,
            operation, envelope, ScalarRemapRange { inputMin[axis], inputMax[axis],
                                                    outputMin[axis], outputMax[axis], flags,
                                                    transform, transformScale, axisSeed,
                                                    noiseOctaves }, birthControlPointIEEE, birthNoise);
    }
    return current;
}

inline float remapLifetimeMultiply (float currentValue, float age, float lifetime,
                                    std::optional<BlendEnvelope> envelope = std::nullopt,
                                    ScalarRemapRange range = {}) {
    if (!std::isfinite (lifetime) || lifetime <= 0.0f) return currentValue;
    const float fraction = age / lifetime;
    return remapScalarMultiply (currentValue, fraction, fraction, envelope, range);
}

// Native 2.8.42 oscillate-alpha/size opcodes 0x1e/0x1f reuse the same
// per-particle random stream component for frequency, phase, and scale.
// Their packed fields are min/span pairs, and phase is added to age before
// frequency multiplication. The caller multiplies its current stream value.
inline float oscillatorMultiplier (float age, float random, float frequencyMin,
                                   float frequencyMax, float phaseMin, float phaseMax,
                                   float scaleMin, float scaleMax,
                                   float normalizedAge,
                                   std::optional<BlendEnvelope> envelope) {
    const float frequency = frequencyMin + random * (frequencyMax - frequencyMin);
    const float phase = phaseMin + random * (phaseMax - phaseMin);
    const float scale = scaleMin + (std::cos ((age + phase) * frequency) + 1.0f)
                                         * random * (scaleMax - scaleMin) * 0.5f;
    return blendedMultiplier (scale, normalizedAge, envelope);
}

// Position opcode 0x1d adds an extra random full-turn phase, then takes the
// cosine difference between the current age and one operator step earlier.
inline float positionOscillationDelta (float age, float step, float random,
                                       float frequencyMin, float frequencyMax,
                                       float phaseMin, float phaseMax,
                                       float scaleMin, float scaleMax,
                                       float instanceSpeed, float envelopeWeight = 1.0f) {
    const float frequency = (frequencyMin + random * (frequencyMax - frequencyMin)) * instanceSpeed;
    const float phase = phaseMin + random * (phaseMax - phaseMin)
                        + random * 6.2831855f;
    const float scale = scaleMin + random * (scaleMax - scaleMin);
    const float angle = (age + phase) * frequency;
    return (std::cos (angle) - std::cos (angle - step * frequency)) * scale * envelopeWeight;
}

// Interpreter 14023fbc0 restores size from stream 0x4f on every invocation
// and restores alpha from 0x66 when its alpha-processing flag is set. This
// happens before all operator records, including each of two half-step passes.
inline void restoreOperatorStreams (float& alpha, float& size, float initialAlpha,
                                    float initialSize, bool resetAlpha) {
    size = initialSize;
    if (resetAlpha) alpha = initialAlpha;
}

// Native 140230650 clamps instanceoverride.rate to 0.01 before multiplying
// the node clock. This does not change an emitter's authored births/second.
inline float nodeRate (float authoredRate) {
    return std::isfinite (authoredRate) ? std::max (authoredRate, 0.01f) : 0.01f;
}

struct EmitterScheduleConfig {
    float rate { 0.0f };
    float delay { 0.0f };
    float duration { 0.0f }; // Zero means unbounded.
    uint32_t instantaneous { 0 };
    bool onePerFrame { false };
    bool periodic { false };
    float minActiveDuration { 0.0f };
    float maxActiveDuration { 0.0f };
    float minOffDelay { 0.0f };
    float maxOffDelay { 0.0f };
    uint32_t maxPerPeriod { 0 }; // Zero means unlimited.
};

template <typename Emitter>
EmitterScheduleConfig scheduleConfig (const Emitter& emitter, float effectiveRate) {
    return {
        .rate = effectiveRate,
        .delay = emitter.delay,
        .duration = emitter.duration,
        .instantaneous = emitter.instantaneous,
        .onePerFrame = (emitter.flags & 2) != 0,
        .periodic = (emitter.flags & 4) != 0,
        .minActiveDuration = emitter.minPeriodicDuration,
        .maxActiveDuration = emitter.maxPeriodicDuration,
        .minOffDelay = emitter.minPeriodicDelay,
        .maxOffDelay = emitter.maxPeriodicDelay,
        .maxPerPeriod = emitter.maxToEmitPerPeriod,
    };
}

struct EmitterScheduleState {
    float delayRemaining { 0.0f };
    float durationRemaining { 0.0f };
    float fractional { 0.0f };
    float periodTimer { 0.0f }; // Positive: active; negative: off.
    uint32_t emittedThisPeriod { 0 };
    uint32_t instantaneousRemaining { 0 };
    bool expired { false };
};

inline EmitterScheduleState initialState (const EmitterScheduleConfig& config) {
    return { config.delay, config.duration, 0.0f, 0.0f, 0, config.instantaneous, false };
}

inline float nativeRandomUnit (std::mt19937& rng) {
    // Native 14007f5b0 uses the top 24 MT19937 bits for a [0,1) float.
    return static_cast<float> (rng () >> 8) * 5.9604644775390625e-8f;
}

inline glm::vec3 colorRandomSample (std::mt19937& rng, glm::vec3 min, glm::vec3 max) {
    // Initializer opcode 3 shares one sampled interpolation factor across RGB
    // (14023b340:389–414), rather than drawing each channel independently.
    const float unit = nativeRandomUnit (rng);
    return min + unit * (max - min);
}

// Native initializer opcode 6 raises its one birth-time random draw before
// interpolating alpha bounds (14023b340). Exponent 1 remains the linear case.
inline float alphaRandomExponentSample (float unit, float min, float max, float exponent,
                                       float instanceAlpha = 1.0f) {
    return (min + std::pow (unit, exponent) * (max - min)) * instanceAlpha;
}

// Native opcode-6 patches both authored turbulent-birth speed bounds from
// the shared root instance. Particle flag 0x10 suppresses this patch.
inline glm::vec2 turbulentBirthSpeedRange (float min, float max, float instanceSpeed,
                                           bool patchSpeedRange) {
    const float gain = patchSpeedRange ? instanceSpeed : 1.0f;
    return { min * gain, max * gain };
}

inline bool emitterCanProduceMore (const EmitterScheduleConfig& config,
                                   const EmitterScheduleState& state) {
    if (state.expired) return false;
    if (state.delayRemaining > 0.0f || state.durationRemaining > 0.0f
        || state.instantaneousRemaining > 0) return true;
    return config.rate > 0.0f && std::isfinite (config.rate);
}

inline bool isAlive (bool active, float age, float lifetime) {
    return active && (lifetime == 0.0f || age <= lifetime);
}

// Linux allocation guard, not a recovered native limit. The shipped corpus
// reaches 25,000; leave headroom while rejecting runaway authored/scripts counts.
constexpr uint32_t MAX_PARTICLE_CAPACITY = 100000;
constexpr size_t MAX_PARTICLE_GEOMETRY_BYTES = 128u * 1024u * 1024u;

inline uint32_t particleCapacity (uint32_t authoredMaxCount) {
    // Native keeps the authored pool size; the instance count descriptor binds
    // emitter production, not allocation (2022437634 high-rate control: 28/27
    // live points with maxcount=40 and count=0.07).
    return std::min (authoredMaxCount, MAX_PARTICLE_CAPACITY);
}

struct RopeGeometry {
    int subdivision;
    size_t floatCount;
    size_t indexCount;
};

inline std::optional<RopeGeometry> ropeGeometry (uint32_t capacity, float rawSubdivision,
                                                  size_t floatsPerVertex) {
    if (!std::isfinite (rawSubdivision) || rawSubdivision < 0.0f
        || rawSubdivision >= static_cast<float> (std::numeric_limits<int>::max ())
        || floatsPerVertex == 0 || floatsPerVertex > MAX_PARTICLE_GEOMETRY_BYTES / (4 * sizeof (float))) {
        return std::nullopt;
    }
    const int subdivision = std::max (1, static_cast<int> (rawSubdivision));
    const uint64_t segments = capacity > 1
        ? static_cast<uint64_t> (capacity - 1) * subdivision : 0;
    const size_t bytesPerSegment = 4 * floatsPerVertex * sizeof (float) + 6 * sizeof (uint32_t);
    if (segments > MAX_PARTICLE_GEOMETRY_BYTES / bytesPerSegment) return std::nullopt;
    return RopeGeometry { subdivision, static_cast<size_t> (segments) * 4 * floatsPerVertex,
                          static_cast<size_t> (segments) * 6 };
}

inline std::optional<RopeGeometry> ropeOrdinaryGeometry (uint32_t capacity,
                                                         float authoredInteriorCount,
                                                         size_t floatsPerVertex) {
    if (!std::isfinite (authoredInteriorCount)) return std::nullopt;
    // 1401c5490 clamps the authored count to [0,32]. 1401d2340 forwards
    // that count unchanged as TRAILSUBDIVISION; the geometry shader inserts
    // that many interior pairs, giving N+1 rendered subsegments.
    const float interiors = std::clamp (authoredInteriorCount, 0.0f, 32.0f);
    return ropeGeometry (capacity, interiors + 1.0f, floatsPerVertex);
}

struct RopeOrdinaryUV {
    float trailLength;
    float positionOffset;
};

inline RopeOrdinaryUV ropeOrdinaryUV (uint32_t aliveCount, uint32_t capacity,
                                    float uniformLifetime, float emissionRate,
                                    float oldestAge, float authoredUVScale,
                                    bool scrolling, bool smoothing,
                                    uint32_t expiredCount = 0) {
    // 1402308a0:73–97 uses original particle count, not generated subdivision
    // vertices. The native node stores reciprocal UV scale at +0x30.
    const float reciprocalUVScale = 1.0f / authoredUVScale;
    const float effectiveSpan = emissionRate * uniformLifetime;
    const float baseRate = effectiveSpan > static_cast<float> (aliveCount)
        ? std::min (static_cast<float> (capacity), emissionRate) : emissionRate;
    const float adjustedSpan = baseRate * uniformLifetime;
    float count = static_cast<float> (aliveCount);
    float offset = 0.0f;
    if (scrolling) {
        count = adjustedSpan - 1.0f;
        offset = static_cast<float> (expiredCount);
    } else if (smoothing && adjustedSpan > 0.0f
               && static_cast<float> (aliveCount) >= adjustedSpan - 1.0f) {
        count = adjustedSpan - 1.0f;
        const float period = 1.0f / baseRate;
        offset = std::clamp ((uniformLifetime - oldestAge) / period, 0.0f, 1.0f) - 1.0f;
    }
    return { count * reciprocalUVScale, offset };
}

// A rope trail belongs to one particle. Keep its samples in a separate row so
// compacting dead particles cannot splice two unrelated trails together.
struct RopeTrailHistory {
    uint32_t segments { 0 };
    std::vector<glm::vec3> positions;
    std::vector<uint16_t> counts;

    RopeTrailHistory () = default;
    RopeTrailHistory (uint32_t capacity, uint32_t count)
        : segments (count), positions (static_cast<size_t> (capacity) * count), counts (capacity, 0) {}

    void born (uint32_t index, const glm::vec3& position) {
        auto row = positions.begin () + static_cast<size_t> (index) * segments;
        std::fill_n (row, segments, position);
        counts[index] = 1;
    }

    void compact (uint32_t destination, uint32_t source) {
        if (destination == source) return;
        std::copy_n (positions.begin () + static_cast<size_t> (source) * segments, segments,
                     positions.begin () + static_cast<size_t> (destination) * segments);
        counts[destination] = counts[source];
    }

    // Keep each particle's history row attached when the compacted active
    // vector is restored to native SoA slot traversal order after a refill.
    void reorder (const std::vector<uint32_t>& sourceAtDestination) {
        std::vector<glm::vec3> rows (sourceAtDestination.size () * segments);
        std::vector<uint16_t> rowCounts (sourceAtDestination.size ());
        for (size_t destination = 0; destination < sourceAtDestination.size (); ++destination) {
            const auto source = sourceAtDestination[destination];
            std::copy_n (positions.begin () + static_cast<size_t> (source) * segments,
                         segments, rows.begin () + destination * segments);
            rowCounts[destination] = counts[source];
        }
        std::copy (rows.begin (), rows.end (), positions.begin ());
        std::copy (rowCounts.begin (), rowCounts.end (), counts.begin ());
    }

    void sample (uint32_t index, const glm::vec3& position) {
        auto row = positions.begin () + static_cast<size_t> (index) * segments;
        std::move_backward (row, row + segments - 1, row + segments);
        *row = position;
        counts[index] = static_cast<uint16_t> (std::min<uint32_t> (segments, counts[index] + 1));
    }

    template <typename PositionAt>
    bool advance (float duration, float interval, float& countdown,
                  uint32_t liveCount, PositionAt&& positionAt) {
        countdown -= duration;
        if (countdown > 0.0f) return false;
        countdown = interval;
        for (uint32_t i = 0; i < liveCount; ++i) sample (i, positionAt (i));
        return true;
    }

    [[nodiscard]] const glm::vec3& at (uint32_t index, uint32_t sample) const {
        return positions[static_cast<size_t> (index) * segments + sample];
    }
};

inline std::optional<RopeGeometry> ropeTrailGeometry (uint32_t capacity, uint32_t segments,
                                                      float rawSubdivision, size_t floatsPerVertex) {
    if (!std::isfinite (rawSubdivision) || segments < 2 || segments > 32) return std::nullopt;
    // One live head plus `segments` stored snapshots makes `segments` quads.
    // Native TRAILSUBDIVISION counts interior points, producing n + 1 quads.
    const float subdivision = std::clamp (rawSubdivision, 0.0f, 32.0f);
    auto one = ropeGeometry (segments + 1, subdivision + 1.0f, floatsPerVertex);
    if (!one) return std::nullopt;
    const size_t bytes = one->floatCount * sizeof (float) + one->indexCount * sizeof (uint32_t);
    if (capacity && bytes > MAX_PARTICLE_GEOMETRY_BYTES / capacity) return std::nullopt;
    return RopeGeometry { one->subdivision, one->floatCount * capacity, one->indexCount * capacity };
}

inline float ropeTrailUVLength (uint16_t sampleCount, float uvScale) {
    // The native renderer record defaults this reciprocal to one and only
    // replaces it for a nonzero authored UV scale.
    const float reciprocal = std::isfinite (uvScale) && uvScale != 0.0f ? 1.0f / uvScale : 1.0f;
    return static_cast<float> (sampleCount) * reciprocal;
}

inline float ropeTrailScrollMaxCount (uint32_t segments, float uvScale) {
    return ropeTrailUVLength (static_cast<uint16_t> (segments - 1), uvScale);
}

inline float ropeTrailFade (uint32_t segment, uint32_t segments,
                            float phase, bool endPoint) {
    const float offset = endPoint ? 1.0f : 0.0f;
    const float normalized = std::clamp (
        (static_cast<float> (segment) - (1.0f - phase) + offset)
            / static_cast<float> (segments - 1), 0.0f, 1.0f);
    return std::sin (normalized * 3.141f);
}

struct RopeTrailUV {
    float start;
    float end;
};

inline RopeTrailUV ropeOrdinarySegmentUV (uint32_t segment, RopeOrdinaryUV uv) {
    const float usableLength = uv.trailLength - 1.0f;
    if (usableLength == 0.0f) return {};
    const float start = 1.0f - (static_cast<float> (segment) + uv.positionOffset) / usableLength;
    return { start, start - 1.0f / usableLength };
}

inline RopeTrailUV ropeTrailSegmentUV (uint32_t segment, float trailLength,
                                      float maxCount, float phase, bool scrolling) {
    float usable;
    float minimum;
    float delta;
    if (scrolling) {
        usable = maxCount;
        if (usable == 0.0f) return {};
        minimum = 1.0f - static_cast<float> (segment) / usable;
        delta = -1.0f / usable;
        if (segment == 0) {
            minimum += delta * (1.0f - phase);
            delta *= phase;
        }
    } else {
        usable = trailLength - 1.0f;
        if (trailLength < maxCount) usable += phase;
        if (usable == 0.0f) return {};
        minimum = (static_cast<float> (segment) - (1.0f - phase)) / usable;
        delta = 1.0f / usable;
        if (segment == 0) {
            minimum = 0.0f;
            delta = phase / usable;
        }
    }
    return { minimum, minimum + delta };
}

inline float ropeTrailSubsegmentUV (RopeTrailUV segmentUV, float fraction) {
    const float smooth = fraction * fraction * (3.0f - 2.0f * fraction);
    return segmentUV.start + (segmentUV.end - segmentUV.start) * smooth;
}

inline glm::vec3 ropeSizedRight (const glm::vec3& eyeDirection,
                                const glm::vec3& tangent, float size) {
    const glm::vec3 vector = glm::cross (eyeDirection, tangent);
    const float squared = glm::dot (vector, vector);
    // Native normalize is undefined at zero length. Keep a finite zero-width
    // defensive result until that raster branch can be compared directly.
    return std::isfinite (squared) && squared > 0.0f
        ? vector * (size / std::sqrt (squared)) : glm::vec3 (0.0f);
}

inline glm::vec3 ropeReflectedSizedRight (const glm::vec3& eyeDirection,
                                         const glm::vec3& tangent, float size) {
    // Particle simulation reflects authored Y. A cross product of two
    // reflected vectors reverses handedness: cross(F e, F t) = -F cross(e,t).
    // Native rope UV U=0 is at position-right; retain that edge association
    // after converting the authored vectors to Linux simulation space.
    return -ropeSizedRight (eyeDirection, tangent, size);
}

inline glm::vec3 ropeInterpolatedRight (const glm::vec3& start, const glm::vec3& end,
                                       float fraction) {
    const float smooth = fraction * fraction * (3.0f - 2.0f * fraction);
    // genericropeparticle.geom interpolates sized endpoint vectors and does
    // not normalize its interior right, so sharp turns may taper the width.
    return glm::mix (start, end, smooth);
}

inline glm::vec3 ropeBezierPosition (glm::vec3 previous, glm::vec3 start,
                                    glm::vec3 end, glm::vec3 next, float fraction) {
    // genericropeparticle.geom smoothsteps each subdivision fraction, then
    // evaluates cubicBezier with 0.15 of the adjacent control deltas.
    const float t = fraction * fraction * (3.0f - 2.0f * fraction);
    const float inverse = 1.0f - t;
    // The vertex shader adds the segment delta before the geometry shader
    // applies its 0.15 control scale.
    const glm::vec3 controlStart = start + (end - previous) * 0.15f;
    const glm::vec3 controlEnd = end + (start - next) * 0.15f;
    return inverse * inverse * inverse * start
        + 3.0f * inverse * inverse * t * controlStart
        + 3.0f * inverse * t * t * controlEnd
        + t * t * t * end;
}

inline std::optional<float> nextSequenceAngle (float rawCount, int& index) {
    // Existing authored count semantics truncate positive fractions to an integer.
    // Invalid dynamic values leave the particle unchanged instead of dividing/modding by zero.
    if (!std::isfinite (rawCount) || rawCount < 1.0f
        || rawCount >= static_cast<float> (std::numeric_limits<int>::max ())) return std::nullopt;
    const int count = static_cast<int> (rawCount);
    if (index < 0 || index >= count) index = 0;
    const float angle = (static_cast<float> (index) / static_cast<float> (count)) * 6.283185307179586f;
    index = index + 1 == count ? 0 : index + 1;
    return angle;
}

// Native initializer 0x0d keeps a floating phase and a signed 1/count step.
// The angle is evaluated before advancing the phase. The repeat branch uses
// the fractional overflow; mirror reflects it and reverses the step.
inline float sequenceAngle (float& phase, float& step, glm::vec2 bounds, bool mirror) {
    const float angle = (bounds.x + phase * (bounds.y - bounds.x)) * 6.2831854820251465f;
    phase += step;
    if (phase > 1.0f) {
        if (mirror) {
            phase = 1.0f - (phase - 1.0f);
            step = -step;
        } else {
            phase = std::fmod (phase, 1.0f);
        }
    } else if (phase < 0.0f) {
        phase = -phase;
        step = -step;
    }
    return angle;
}

inline void resetSequencePhase (float& phase, float& /* signedStep */) {
    // Native 14022f790/14022f6c0 writes record +0x08 only. A mirrored
    // sequence retains its signed +0x04 step across this reset.
    phase = 0.0f;
}

struct SequenceOrbit {
    glm::vec3 position;
    glm::vec3 radial;
    glm::vec3 tangent;
    glm::vec3 axis;
};

struct SequenceBasis {
    glm::vec3 axis;
    glm::vec3 b;
    glm::vec3 c;
};

inline SequenceBasis sequenceBasis (glm::vec3 authoredAxis) {
    // Factory 1401c19e0 builds this orthonormal frame in authored coordinates.
    // Opcode 0x0d uses each vector directly; the Linux particle streams reflect
    // authored Y, so reflect the finished frame before orbiting in that space.
    const float axisLength = glm::length (authoredAxis);
    const glm::vec3 axis = std::isfinite (axisLength) && axisLength > 0.0f
        ? authoredAxis / axisLength : glm::vec3 (0.0f, 0.0f, 1.0f);
    const glm::vec3 b = axis.x == 0.0f && axis.y == 0.0f
        ? glm::vec3 (1.0f, 0.0f, 0.0f)
        : glm::normalize (glm::cross (axis, glm::vec3 (0.0f, 0.0f, 1.0f)));
    const glm::vec3 c = axis.x == 0.0f && axis.y == 0.0f
        ? glm::vec3 (0.0f, 1.0f, 0.0f) : glm::normalize (glm::cross (axis, b));
    return {toSimulationVector (axis), toSimulationVector (b), toSimulationVector (c)};
}

inline SequenceOrbit sequenceOrbitInBasis (glm::vec3 position, glm::vec3 center,
                                           SequenceBasis basis, float angle) {
    const glm::vec3 radial = std::cos (angle) * basis.c + std::sin (angle) * basis.b;
    const glm::vec3 tangent = -std::sin (angle) * basis.c + std::cos (angle) * basis.b;
    const glm::vec3 delta = position - center;
    const float axial = glm::dot (delta, basis.axis);
    const float radius = glm::length (delta - axial * basis.axis);
    return {center + axial * basis.axis + radius * radial, radial, tangent, basis.axis};
}

inline SequenceOrbit sequenceOrbit (glm::vec3 position, glm::vec3 center,
                                    glm::vec3 authoredAxis, float angle) {
    return sequenceOrbitInBasis (position, center, sequenceBasis (authoredAxis), angle);
}

template <typename Sample>
inline glm::vec3 sequenceSpeed (glm::vec3 low, glm::vec3 high, Sample sample) {
    // Opcode 0x0d consumes native random words for axial Z, tangent X,
    // radial Y. Return ordinary XYZ components for the orbit combiner.
    const float z = sample (low.z, high.z);
    const float x = sample (low.x, high.x);
    const float y = sample (low.y, high.y);
    return {x, y, z};
}

// Invalid dynamic overrides disable rate emission; finite positive overflow
// saturates, so no float-to-integer conversion can operate on infinity.
inline float effectiveRate (float authored, float overrideRate) {
    if (!std::isfinite (authored) || !std::isfinite (overrideRate)
        || authored < 0.0f || overrideRate < 0.0f) {
        return 0.0f;
    }
    const double product = static_cast<double> (authored) * overrideRate;
    return static_cast<float> (std::min (product,
        static_cast<double> (std::numeric_limits<float>::max ())));
}

template <typename RandomFloat>
uint32_t advanceEmitter (const EmitterScheduleConfig& config, EmitterScheduleState& state,
                         float dt, uint32_t available, RandomFloat&& randomFloat,
                         bool* periodRestarted = nullptr) {
    if (periodRestarted) *periodRestarted = false;
    if (!std::isfinite (dt) || dt <= 0.0f || state.expired) {
        return 0;
    }

    uint32_t requested = 0;
    if (state.delayRemaining <= 0.0f && available > 0) {
        bool active = true;
        if (config.periodic) {
            if (state.periodTimer <= 0.0f) {
                state.periodTimer += dt;
                if (state.periodTimer >= 0.0f) {
                    state.periodTimer = randomFloat (config.minActiveDuration,
                                                     config.maxActiveDuration);
                    state.emittedThisPeriod = 0;
                    state.instantaneousRemaining = config.instantaneous;
                    if (periodRestarted) *periodRestarted = true;
                } else {
                    active = false;
                }
            } else {
                state.periodTimer -= dt;
                if (state.periodTimer < 0.0f) {
                    state.periodTimer = -randomFloat (config.minOffDelay,
                                                       config.maxOffDelay);
                }
            }
        }

        if (active) {
            requested = state.instantaneousRemaining;
            state.instantaneousRemaining = 0;
            const float rate = std::isfinite (config.rate) && config.rate >= 0.0f
                ? config.rate : 0.0f;
            const float accumulated = state.fractional + rate * dt;
            uint32_t whole = 0;
            if (std::isfinite (accumulated)) {
                const float integer = std::max (0.0f, std::floor (accumulated));
                state.fractional = accumulated - integer
                    - static_cast<float> (requested);
                if (integer >= static_cast<float> (std::numeric_limits<uint32_t>::max ())) {
                    whole = std::numeric_limits<uint32_t>::max ();
                } else if (integer > 0.0f) {
                    whole = static_cast<uint32_t> (integer);
                }
            } else {
                // A finite rate*dt can overflow float. All representable
                // births are requested; its fractional part is unavailable.
                whole = std::numeric_limits<uint32_t>::max ();
                state.fractional = 0.0f;
            }
            uint32_t rateCount = config.onePerFrame ? std::min (whole, 1u) : whole;
            if (config.periodic && config.maxPerPeriod > 0) {
                const uint32_t room = state.emittedThisPeriod < config.maxPerPeriod
                    ? config.maxPerPeriod - state.emittedThisPeriod : 0;
                rateCount = std::min (rateCount, room);
                // Native 2.8.42 advances record +0x3c by the capped rate
                // request before clamping births to free slots. Keep that
                // order; partial-capacity runtime capture is still pending.
                state.emittedThisPeriod += rateCount;
            }
            requested = rateCount > std::numeric_limits<uint32_t>::max () - requested
                ? std::numeric_limits<uint32_t>::max () : requested + rateCount;
        }
    }

    // These clocks run at the end of the emitter call, including when the pool is full.
    if (state.delayRemaining > 0.0f) {
        state.delayRemaining -= dt;
    } else if (config.duration > 0.0f) {
        state.durationRemaining -= dt;
        if (state.durationRemaining <= 0.0f) {
            state.durationRemaining = -1.0f;
            state.expired = true;
        }
    }
    return std::min (requested, available);
}

// A script-requested birth enters native 1402378a0 with dt=0 and a positive
// per-emitter count. The inner scheduler consumes pending instantaneous births
// and borrows that count from fractional rate credit before pool clipping.
// Keep this distinct from an ordinary positive-dt tick: the latter has its
// own delay/duration and periodic clock transitions.
template <typename RandomFloat>
uint32_t advanceEmitterForced (const EmitterScheduleConfig& config,
                               EmitterScheduleState& state, uint32_t forced,
                               uint32_t available, RandomFloat&& randomFloat,
                               bool* periodRestarted = nullptr) {
    if (periodRestarted) *periodRestarted = false;
    if (forced == 0 || available == 0 || state.expired || state.delayRemaining > 0.0f)
        return 0;
    if (config.periodic && state.periodTimer == 0.0f) {
        state.periodTimer = randomFloat (config.minActiveDuration,
                                         config.maxActiveDuration);
        state.emittedThisPeriod = 0;
        state.instantaneousRemaining = config.instantaneous;
        if (periodRestarted) *periodRestarted = true;
    }
    const uint64_t pending = static_cast<uint64_t> (forced)
        + state.instantaneousRemaining;
    state.instantaneousRemaining = 0;
    uint64_t requested = pending;
    uint32_t whole = 0;
    if (state.fractional >= 1.0f) {
        const float integer = std::floor (state.fractional);
        whole = integer >= static_cast<float> (std::numeric_limits<uint32_t>::max ())
            ? std::numeric_limits<uint32_t>::max () : static_cast<uint32_t> (integer);
        uint32_t rateCount = config.onePerFrame ? std::min (whole, 1u) : whole;
        if (config.periodic && config.maxPerPeriod > 0) {
            const uint32_t room = state.emittedThisPeriod < config.maxPerPeriod
                ? config.maxPerPeriod - state.emittedThisPeriod : 0;
            rateCount = std::min (rateCount, room);
            state.emittedThisPeriod += rateCount;
        }
        requested += rateCount;
    }
    // 1402378a0 stores credit after subtracting the *uncapped* whole-rate
    // amount and the full forced+instant count, even if the slot pool clips.
    state.fractional -= static_cast<float> (pending + whole);
    return static_cast<uint32_t> (std::min<uint64_t> (requested, available));
}

} // namespace WallpaperEngine::Render::Objects::ParticleCore
