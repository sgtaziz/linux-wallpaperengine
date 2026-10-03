#pragma once

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

namespace WallpaperEngine::Audio {

/**
 * Per-channel level of a mono clip. Native renders mono buffers through
 * OpenAL Soft 1.21's stereo decoder (W 0.5, Y 0.2887*sqrt(3), X 0.0552*sqrt(3));
 * a non-spatial mono clip sits at the source default position, which gives
 * 0.5 + 0.0957 per channel relative to a stereo channel. Stereo clips are
 * played as is and are never spatialized.
 */
struct MonoChannelGains {
    float left;
    float right;
};

inline constexpr float kStereoDecoderSide = 0.5f;
inline constexpr float kStereoDecoderFront = 0.0957f;
inline constexpr MonoChannelGains kNonSpatialMonoGains {
    kStereoDecoderSide + kStereoDecoderFront, kStereoDecoderSide + kStereoDecoderFront
};
/** Native places the orthographic listener at z = 2000. */
inline constexpr float kOrthographicListenerZ = 2000.0f;

/** The scene camera as the native listener sees it. */
struct SoundListener {
    /** Camera eye; orthographic scenes add the canvas half size and use z = 2000. */
    glm::vec3 position {0.0f};
    /** Unit right vector of the view (screen +x). */
    glm::vec3 right {1.0f, 0.0f, 0.0f};
    /** Unit view direction (from the eye toward the centre). */
    glm::vec3 forward {0.0f, 0.0f, -1.0f};
    bool orthographic = false;
    float width = 0.0f;
    float height = 0.0f;
};

/**
 * Builds the listener from the current camera pose. Native copies
 * the scene eye and, for orthographic scenes, adds half the
 * canvas size and sets z to 2000; then measures sources from it.
 */
inline SoundListener soundListenerForCamera (
    const glm::vec3& eye, const glm::vec3& center, const glm::vec3& up, bool orthographic, float width,
    float height
) {
    SoundListener listener;
    listener.orthographic = orthographic;
    listener.width = width;
    listener.height = height;
    listener.position = orthographic
        ? glm::vec3 (eye.x + width * 0.5f, eye.y + height * 0.5f, kOrthographicListenerZ) : eye;
    const glm::vec3 view = center - eye;
    if (glm::length (view) > 0.0f) listener.forward = glm::normalize (view);
    const glm::vec3 side = glm::cross (listener.forward, up);
    if (glm::length (side) > 0.0f) listener.right = glm::normalize (side);
    return listener;
}

/** AL_INVERSE_DISTANCE_CLAMPED with reference = mindistance and rolloff = attenuation. */
inline float inverseDistanceClampedGain (float distance, float referenceDistance, float rolloff) {
    if (!(referenceDistance > 0.0f) || !std::isfinite (distance)) return 1.0f;
    const float clamped = std::max (distance, referenceDistance);
    const float scaled = referenceDistance + rolloff * (clamped - referenceDistance);
    return scaled > 0.0f ? referenceDistance / scaled : 1.0f;
}

/**
 * Spatial gains for a mono clip whose world origin is @p origin
 * The pan uses the unit direction toward the
 * source: Y is its lateral component toward screen-left and F its
 * component along the view direction. The distance is the world distance
 * in perspective scenes and max(0, q - z) / q with q = (W + H) / 4 in
 * orthographic scenes.
 */
inline MonoChannelGains spatialMonoGains (
    const glm::vec3& origin, const SoundListener& listener, float minDistance, float attenuation
) {
    const glm::vec3 offset = origin - listener.position;
    const float length = glm::length (offset);
    float distance = length;
    if (listener.orthographic) {
        const float quarter = (listener.width + listener.height) * 0.25f;
        distance = quarter > 0.0f ? std::max (0.0f, quarter - origin.z) / quarter : 0.0f;
    }
    // A zero source vector is the source default position: the same level
    // as a non-spatial clip, at the clamped reference distance.
    if (!(length > 0.0f) || !(distance > 0.0f)) return kNonSpatialMonoGains;
    const glm::vec3 direction = offset / length;
    const float lateral = -glm::dot (direction, listener.right);
    const float front = glm::dot (direction, listener.forward);
    const float gain = inverseDistanceClampedGain (distance, minDistance, attenuation);
    return {
        gain * (kStereoDecoderSide + kStereoDecoderSide * lateral - kStereoDecoderFront * front),
        gain * (kStereoDecoderSide - kStereoDecoderSide * lateral - kStereoDecoderFront * front),
    };
}

} // namespace WallpaperEngine::Audio
