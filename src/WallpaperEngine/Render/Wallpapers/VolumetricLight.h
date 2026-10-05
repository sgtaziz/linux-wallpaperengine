#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace WallpaperEngine::Render::Wallpapers {
/**
 * Native volumetric quality: disabled 0, low 1,
 * medium 2, high 3, ultra 4. Linux has no user setting for it; the renderer
 * uses the native capture configuration's "ultra".
 */
constexpr int kVolumetricQuality = 4;

/** the light buffers use 1/4 of the scene size at quality >= 3, else 1/8. */
inline int volumetricBufferDivisor (int quality) { return quality >= 3 ? 4 : 8; }

inline glm::ivec2 volumetricBufferSize (int width, int height, int quality) {
    const int divisor = volumetricBufferDivisor (quality);
    return {std::max (1, width / divisor), std::max (1, height / divisor)};
}

/** Uniforms of one point-light volume. */
struct PointVolumeUniforms {
    /** g_AltViewProjectionMatrix: the unit volume mesh scaled by radius at the light origin. */
    glm::mat4 volume {1.0f};
    /** g_RenderVar1: radius * 0.99, cos(innercone), cos(outercone), intensity. */
    glm::vec4 spotParams {0.0f};
    /** g_RenderVar2: light origin, density. */
    glm::vec4 originDensity {0.0f};
    /** g_RenderVar4: raw color (not multiplied by intensity), volumetricsexponent. */
    glm::vec4 colorExponent {0.0f};
};

// Point lights keep the modern-light cone defaults (20 and 30 degrees) in the
// spot slots; the POINTLIGHT shader path never reads them.
inline PointVolumeUniforms packPointVolume (
    const glm::vec3& origin, const glm::vec3& color, float intensity, float radius,
    float density, float volumetricsExponent, float innerCone = 20.0f, float outerCone = 30.0f
) {
    PointVolumeUniforms result;
    result.volume = glm::translate (glm::mat4 (1.0f), origin) * glm::scale (glm::mat4 (1.0f), glm::vec3 (radius));
    result.spotParams = {radius * 0.99f, std::cos (glm::radians (innerCone)), std::cos (glm::radians (outerCone)), intensity};
    result.originDensity = {origin, density};
    result.colorExponent = {color, volumetricsExponent};
    return result;
}

/**
 * draws the full-screen variant instead of the front faces when
 * the eye, nudged 0.2 along the view, lies inside a point volume.
 */
inline bool eyeInsidePointVolume (const glm::vec3& eye, const glm::vec3& forward, const glm::vec3& origin, float radius) {
    const glm::vec3 delta = eye + forward * 0.2f - origin;
    return glm::dot (delta, delta) < radius * radius;
}

/**
 * finishes a pending volumetric batch before drawing an
 * object only when that object is drawn (visible) and is not a light; lights
 * and invisible objects leave the batch open.
 */
inline bool volumetricBatchEndsBefore (bool light, bool visible) { return !light && visible; }
} // namespace WallpaperEngine::Render::Wallpapers
