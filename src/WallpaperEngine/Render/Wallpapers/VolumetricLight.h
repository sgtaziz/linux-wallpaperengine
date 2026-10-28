#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

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

/** Uniforms of one light volume. */
struct VolumeUniforms {
    /**
     * g_AltViewProjectionMatrix: for a point light the unit volume mesh
     * scaled by radius at the light origin; for a spot light the inverse of
     * the light's view-projection, taking the
     * light-NDC cone mesh to world space.
     */
    glm::mat4 volume {1.0f};
    /** g_AltModelMatrix: the light view-projection, read by spot cookie/shadow UVs only. */
    glm::mat4 lightProjection {1.0f};
    /** g_RenderVar3: the spot forward (first four floats of the node matrix); unused by points. */
    glm::vec4 forward {0.0f};
    /** g_RenderVar1: radius * 0.99, cos(innercone), cos(outercone), intensity. */
    glm::vec4 spotParams {0.0f};
    /** g_RenderVar2: light origin, density. */
    glm::vec4 originDensity {0.0f};
    /** g_RenderVar4: raw color (not multiplied by intensity), volumetricsexponent. */
    glm::vec4 colorExponent {0.0f};
};
using PointVolumeUniforms = VolumeUniforms;

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
 * Native draws the full-screen variant instead of the front faces when
 * the eye, nudged 0.2 along the view, lies inside a point volume.
 */
inline bool eyeInsidePointVolume (const glm::vec3& eye, const glm::vec3& forward, const glm::vec3& origin, float radius) {
    const glm::vec3 delta = eye + forward * 0.2f - origin;
    return glm::dot (delta, delta) < radius * radius;
}


/** Which volume mesh a light draws: sphere for type 0, the 32-segment cone for spots. */
enum class VolumeShape { Point, Spot };

/**
 * Spot-light volume.
 * g_RenderVar1.y/z are cos(innercone) / cos(outercone) and g_RenderVar3 the
 * node matrix's first column (the spot forward, not normalised).
 */
inline VolumeUniforms packSpotVolume (
    const glm::mat4& authoredMatrix, const glm::mat4& lightProjection, const glm::vec3& color, float intensity,
    float radius, float innerCone, float outerCone, float density, float volumetricsExponent
) {
    VolumeUniforms result;
    result.volume = glm::inverse (lightProjection);
    result.lightProjection = lightProjection;
    result.forward = authoredMatrix[0];
    result.spotParams = {radius * 0.99f, std::cos (glm::radians (innerCone)), std::cos (glm::radians (outerCone)), intensity};
    result.originDensity = {glm::vec3 (authoredMatrix[3]), density};
    result.colorExponent = {color, volumetricsExponent};
    return result;
}

/**
 * The spot volume mesh in light NDC: a 32-segment
 * cylinder of unit-circle cross section between the near and far NDC depths,
 * capped by fans around the (0.5, 0.5) centre vertices 0 (near) and 1 (far).
 * Depth order follows the light projection (native: far / 
 * near; Linux's spot projection is reversed, far 0 and near 1). The vertex
 * shader scales x and y by 0.99 for non-point lights.
 */
struct VolumeMesh {
    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices;
};

inline VolumeMesh buildSpotVolumeMesh (float farDepth, float nearDepth) {
    constexpr int kSegments = 32;
    constexpr float kStep = 6.2831854820251465f / 32.0f;
    VolumeMesh mesh;
    mesh.vertices.push_back ({0.5f, 0.5f, nearDepth});
    mesh.vertices.push_back ({0.5f, 0.5f, farDepth});
    for (int i = 0; i < kSegments; ++i) {
        const float angle = float (i) * kStep, next = angle + kStep;
        const uint32_t base = uint32_t (mesh.vertices.size ());
        mesh.vertices.push_back ({std::cos (angle), -std::sin (angle), farDepth});
        mesh.vertices.push_back ({std::cos (next), -std::sin (next), farDepth});
        mesh.vertices.push_back ({std::cos (angle), -std::sin (angle), nearDepth});
        mesh.vertices.push_back ({std::cos (next), -std::sin (next), nearDepth});
        mesh.indices.insert (mesh.indices.end (),
            {base + 2, base, base + 1, base + 2, base + 1, base + 3, 1, base + 1, base, 0, base + 2, base + 3});
    }
    return mesh;
}

/**
 * (spot, no cookie): the eye, nudged 0.2 along the view, is inside
 * when its axial distance t along the normalised spot axis lies in (0, radius]
 * and its distance from the axis is at most t / radius times the far-plane
 * cone radius (the world distance between the unprojected far-plane centre and
 * the point at NDC y = 1).
 */
inline bool eyeInsideSpotVolume (
    const glm::vec3& eye, const glm::vec3& forward, const glm::mat4& authoredMatrix, const glm::mat4& inverseProjection,
    float farNdcDepth, float radius
) {
    const glm::vec3 delta = eye + forward * 0.2f - glm::vec3 (authoredMatrix[3]);
    const glm::vec4 column = authoredMatrix[0];
    const float length = std::sqrt (glm::dot (column, column));
    const glm::vec3 axis = glm::vec3 (column) / length;
    const float along = glm::dot (axis, delta);
    const float across = glm::length (delta - axis * along);
    const auto unproject = [&] (float y) {
        const glm::vec4 p = inverseProjection * glm::vec4 (0.0f, y, farNdcDepth, 1.0f);
        return glm::vec3 (p) / p.w;
    };
    const float farRadius = glm::length (unproject (1.0f) - unproject (0.0f));
    return !(along <= 0.0f || radius < along || (along / radius) * farRadius < across);
}

/**
 * Native RenderDoc replays (zr3-vol ortho fixtures vs-spot / vs-spotin / vs-inside) draw the volume mesh even with the eye inside, never the
 * volumetrics_fullscreen triangle; perspective captures (vs-inside-persp, vs-spotin-persp) take the full-screen march. The switch is therefore
 * the inside test AND a perspective camera.
 */
inline bool volumetricFullscreen (bool orthographicCamera, bool eyeInside) { return !orthographicCamera && eyeInside; }

/**
 * Native finishes a pending volumetric batch before drawing an
 * object only when that object is drawn (visible) and is not a light; lights
 * and invisible objects leave the batch open.
 */
inline bool volumetricBatchEndsBefore (bool light, bool visible) { return !light && visible; }
} // namespace WallpaperEngine::Render::Wallpapers
