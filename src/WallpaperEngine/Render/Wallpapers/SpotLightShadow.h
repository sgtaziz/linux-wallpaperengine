#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>
#include <glm/glm.hpp>

#include "SpotLightUniforms.h"

namespace WallpaperEngine::Render::Wallpapers {

// Native constructs a spot projection in authored world space.
// Its basis uses the same single Newton step as the original renderer; using
// glm::lookAt would normalize again and change the recovered GPU matrices.
inline std::optional<glm::mat4> spotLightProjection (
    const glm::mat4& world, float outerCone, float radius, bool orthographicScene
) {
    glm::mat4 view (1.0f);
    const glm::vec3 basis[] {glm::vec3 (world[2]), glm::vec3 (world[1]), -glm::vec3 (world[0])};
    const auto origin = glm::vec3 (world[3]);
    for (int row = 0; row < 3; ++row) {
        const float squared = glm::dot (basis[row], basis[row]);
        if (!(squared > 0) || !std::isfinite (squared)) return std::nullopt;
        float inverse = std::bit_cast<float> (0x5f375a86u - (std::bit_cast<uint32_t> (squared) >> 1));
        inverse = (1.5f - squared * 0.5f * inverse * inverse) * inverse;
        const auto axis = basis[row] * inverse;
        for (int column = 0; column < 3; ++column) view[column][row] = axis[column];
        view[3][row] = -glm::dot (axis, origin);
    }
    const float near = orthographicScene ? 1.0f : 0.05f;
    const float far = std::max (radius, near + 0.01f);
    const float extent = std::tan (glm::radians (outerCone));
    if (!(extent > 0) || !std::isfinite (extent) || !std::isfinite (far)) return std::nullopt;
    // Reversed zero-to-one depth: the original atlas clears to zero and uses
    // GREATER for both depth writes and comparison sampling.
    glm::mat4 projection (0.0f);
    projection[0][0] = projection[1][1] = 1.0f / extent;
    projection[2][2] = near / (far - near);
    projection[3][2] = near * far / (far - near);
    projection[2][3] = -1.0f;
    return projection * view;
}

inline glm::mat4 spotLightCasterProjection (glm::mat4 projection) {
    // Native subtracts from this matrix element before the
    // depth draw, while receivers retain the original sampling projection.
    projection[3][2] -= 0.0005f;
    return projection;
}

enum class SpotLightCategory { Plain = 0, Shadow = 1, Cookie = 2, ShadowCookie = 3 };

inline SpotLightCategory spotLightCategory (bool shadow, bool cookie) {
    return SpotLightCategory ((shadow ? 1 : 0) | (cookie ? 2 : 0));
}

// Native context vector 0x160 (published as g_ViewForward) is the live view
// direction: the negated third row of the view rotation the frame renders with.
inline glm::vec3 viewForward (const glm::mat4& view) {
    const glm::vec3 forward (-view[0][2], -view[1][2], -view[2][2]);
    const float length = glm::length (forward);
    return length > 0 && std::isfinite (length) ? forward / length : glm::vec3 (0.0f);
}

// Native sorts the scene lights every frame (, stable for
// fewer than 33 lights) by type, then by their raw castshadow|usecookie flags
// descending, then by dot(authored origin, context vector 0x160) ascending.
// The context vector is the live view forward (viewForward).
inline bool spotLightPrecedes (int leftFlags, float leftDepth, int rightFlags, float rightDepth) {
    if (leftFlags != rightFlags) return leftFlags > rightFlags;
    return leftDepth < rightDepth;
}

// Native keeps one write cursor per category, starting at the
// serialized slot ranges both, cookie, shadow, then plain (the order of the
// generated loops), and a single counter of total spot lights. A
// category cursor is not bounded by its own range: extra lights continue into
// the following range. Natively a cursor past the total spot count writes
// into the next uniform array; those writes are dropped here.
class SpotLightSlotAllocator {
public:
    struct Counts { int total, shadow, cookie, shadowCookie; };
    explicit SpotLightSlotAllocator (const Counts& counts) : m_remaining (counts.total), m_total (counts.total) {
        m_cursor[3] = 0;
        m_cursor[2] = counts.shadowCookie;
        m_cursor[1] = counts.shadowCookie + counts.cookie;
        m_cursor[0] = counts.shadowCookie + counts.cookie + counts.shadow;
    }
    std::optional<int> next (SpotLightCategory category) {
        if (m_remaining <= 0) return std::nullopt;
        --m_remaining;
        const int slot = m_cursor[int (category)]++;
        if (slot >= m_total) return std::nullopt;
        return slot;
    }
private:
    int m_remaining, m_total;
    int m_cursor[4] {};
};

// Native context byte 0x1ac selects the shadow quality. The original host
// in the native captures was configured with shadows=ultra, which selects
// quality four; Linux has no separate host shadow-quality setting.
constexpr int nativeShadowQuality = 4;

// Native: qualities one and two use 256, three 512, four 1024.
inline uint32_t shadowAtlasTileSize (int quality) {
    return quality == 3 ? 512u : quality == 4 ? 1024u : 256u;
}

struct ShadowAtlasTile {
    uint32_t x = 0, y = 0, size = 0;
};

struct ShadowAtlasLayout {
    uint32_t width = 2, height = 2;
    /** Tiles in request order. */
    std::vector<ShadowAtlasTile> tiles;
};

// Native orders requests by tile size (insertion sort through
// stable for equal keys), then places them with a shelf packer:
// segments start as one zero-height 8192-wide shelf, each segment is grown
// to the request height, and a failed segment opens a new shelf at its top.
// A placed tile also opens the segment below it in the same column.
inline ShadowAtlasLayout packShadowAtlas (const std::vector<uint32_t>& sizes) {
    struct Segment { uint32_t x0, x1, y0, y1; };
    std::vector<size_t> order (sizes.size ());
    for (size_t i = 0; i < order.size (); ++i) order[i] = i;
    std::stable_sort (order.begin (), order.end (),
        [&sizes] (size_t left, size_t right) { return sizes[left] > sizes[right]; });
    ShadowAtlasLayout layout;
    layout.tiles.resize (sizes.size ());
    std::vector<Segment> segments {{0, 0x2000, 0, 0}};
    uint32_t width = 0, height = 0;
    for (const size_t request : order) {
        const uint32_t size = sizes[request];
        for (size_t i = 0; i < segments.size (); ++i) {
            auto& segment = segments[i];
            segment.y1 = std::max (segment.y1, segment.y0 + size);
            if (size <= segment.x1 - segment.x0 && size <= segment.y1 - segment.y0) {
                layout.tiles[request] = {segment.x0, segment.y0, size};
                width = std::max (width, segment.x0 + size);
                height = std::max (height, segment.y0 + size);
                const Segment below {segment.x0, segment.x0 + size, segment.y0 + size, segment.y1};
                segment.x0 += size;
                segments.push_back (below);
                break;
            }
            const uint32_t top = segment.y1;
            segments.push_back ({0, 0x2000, top, top});
        }
    }
    layout.width = std::max (2u, width);
    layout.height = std::max (2u, height);
    return layout;
}

// Atlas transforms divide by the allocated atlas, which only grows natively.
inline glm::vec4 shadowAtlasTransform (const ShadowAtlasTile& tile, uint32_t width, uint32_t height) {
    return {float (tile.x) / float (width), float (tile.y) / float (height),
            float (tile.size) / float (width), float (tile.size) / float (height)};
}

inline glm::mat4 spotLightGLCasterProjection (const glm::mat4& projection) {
    auto raster = spotLightCasterProjection (projection);
    // Native Vulkan presents the atlas through a negative-height viewport.
    // GL has positive-height viewports and clip depth in [-1, 1]. Sampling
    // still uses the unchanged native matrix and zero-to-one comparison depth.
    for (int column = 0; column < 4; ++column) {
        raster[column][1] = -raster[column][1];
        raster[column][2] = 2.0f * raster[column][2] - raster[column][3];
    }
    return raster;
}

/** One visible spot light, in scene registration order. */
struct SpotLightInput {
    bool castShadow = false;
    bool useCookie = false;
    /** dot(authored local origin, view forward); native. */
    float depth = 0.0f;
    SpotLightUniforms uniforms {glm::vec4 (0.0f), glm::vec4 (0.0f), glm::vec4 (0.0f), glm::vec4 (0.0f)};
    /** Sampling projection (spotLightProjection); empty when degenerate. */
    std::optional<glm::mat4> projection;
};

struct SpotLightPacking {
    std::vector<SpotLightUniforms> slots;
    std::vector<glm::mat4> projections;
    std::vector<glm::vec4> transforms;
    struct View {
        int slot;
        glm::mat4 casterProjection;
        ShadowAtlasTile tile;
    };
    std::vector<View> views;
    /** Input index whose cookie is bound, or -1. */
    int cookie = -1;
    /** Allocated atlas after growth; unchanged without shadow views. */
    uint32_t atlasWidth = 0, atlasHeight = 0;
};

// Per-frame spot producer: sort, category
// cursors, the last slotted cookie light selects the single cookie alias,
// feature projections while the feature budget lasts, one tile per shadowing
// light, and transforms divided by the grown atlas.
inline SpotLightPacking packSpotLights (
    const std::vector<SpotLightInput>& lights, const SpotLightSlotAllocator::Counts& counts,
    int quality, uint32_t atlasWidth, uint32_t atlasHeight
) {
    const int features = counts.shadow + counts.cookie + counts.shadowCookie;
    SpotLightPacking result;
    const glm::vec4 zero (0.0f);
    result.slots.resize (counts.total, SpotLightUniforms {zero, zero, zero, zero});
    result.projections.resize (features, glm::mat4 (0.0f));
    result.transforms.resize (features, glm::vec4 (0.0f));
    result.atlasWidth = atlasWidth;
    result.atlasHeight = atlasHeight;
    std::vector<size_t> order (lights.size ());
    for (size_t i = 0; i < order.size (); ++i) order[i] = i;
    const auto flags = [&lights] (size_t i) { return (lights[i].castShadow ? 1 : 0) | (lights[i].useCookie ? 2 : 0); };
    std::stable_sort (order.begin (), order.end (), [&] (size_t left, size_t right) {
        return spotLightPrecedes (flags (left), lights[left].depth, flags (right), lights[right].depth);
    });
    SpotLightSlotAllocator slots (counts);
    int remaining = features;
    for (const size_t index : order) {
        const auto& light = lights[index];
        // castshadow only applies with nonzero shadow quality.
        const bool shadow = light.castShadow && quality != 0;
        const auto slot = slots.next (spotLightCategory (shadow, light.useCookie));
        if (!slot) continue;
        result.slots[*slot] = light.uniforms;
        if (light.useCookie) result.cookie = int (index);
        if (!(shadow || light.useCookie) || remaining == 0 || *slot >= features) continue;
        --remaining;
        if (!light.projection) continue;
        result.projections[*slot] = *light.projection;
        if (shadow) result.views.push_back ({*slot, spotLightGLCasterProjection (*light.projection), {}});
    }
    if (result.views.empty ()) return result;
    const auto layout = packShadowAtlas (std::vector<uint32_t> (result.views.size (), shadowAtlasTileSize (quality)));
    // The atlas only grows, to exactly the requested size when too small.
    if (atlasWidth < layout.width || atlasHeight < layout.height) {
        result.atlasWidth = layout.width;
        result.atlasHeight = layout.height;
    }
    for (size_t view = 0; view < result.views.size (); ++view) {
        result.views[view].tile = layout.tiles[view];
        result.transforms[result.views[view].slot] =
            shadowAtlasTransform (layout.tiles[view], result.atlasWidth, result.atlasHeight);
    }
    return result;
}

/** One visible tube light, in scene registration order. */
struct TubeLightInput {
    /** Raw castshadow|usecookie flags (sort key only; tubes draw unshadowed). */
    int flags = 0;
    /** dot(authored local origin, view forward); native. */
    float depth = 0.0f;
    TubeLightUniforms uniforms {glm::vec4 (0.0f), glm::vec4 (0.0f), glm::vec4 (0.0f)};
};

// Native fills tube slots sort order while its
// serialized tube count (lightconfig bits 8-11) lasts; unused slots stay zero.
inline std::vector<TubeLightUniforms> packTubeLights (const std::vector<TubeLightInput>& lights, int slots) {
    const glm::vec4 zero (0.0f);
    std::vector<TubeLightUniforms> result (std::max (slots, 0), TubeLightUniforms {zero, zero, zero});
    std::vector<size_t> order (lights.size ());
    for (size_t i = 0; i < order.size (); ++i) order[i] = i;
    std::stable_sort (order.begin (), order.end (), [&] (size_t left, size_t right) {
        return spotLightPrecedes (lights[left].flags, lights[left].depth, lights[right].flags, lights[right].depth);
    });
    for (size_t slot = 0; slot < result.size () && slot < order.size (); ++slot)
        result[slot] = lights[order[slot]].uniforms;
    return result;
}

} // namespace WallpaperEngine::Render::Wallpapers
