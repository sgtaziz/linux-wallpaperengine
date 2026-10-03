#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <glm/vec4.hpp>

namespace WallpaperEngine::Render::Objects::ParticleAtlas {

// Texel rectangle (x, y, width, height) inside the uploaded mip0 page.
using TexelRect = glm::vec4;

struct Source {
    /** Dimensions of the uploaded mip0 page (the native texture object). */
    uint32_t mipWidth = 0;
    uint32_t mipHeight = 0;
    /** TEXI image dimensions: the authored data inside that page. */
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
    /** TEXS frame rectangles, in mip0 texels, in frame order. */
    std::vector<TexelRect> authored;
};

enum class WindowSource : uint8_t {
    /** Native window; within one texel of the authored TEXS rect. */
    Native,
    /** Deliberate divergence: native mis-indexes this frame; the authored TEXS rect is kept. */
    Authored,
    /** The authored rect has no coverage but the native window has. */
    NativeOverEmptyAuthored,
    /** The TEXS layout places frames off the page; native windows tile it. */
    NativeOverOffPageLayout,
};

struct Layout {
    /** Normalized sampling window (u, v, width, height) per frame. */
    std::vector<glm::vec4> windows;
    std::vector<WindowSource> sources;
    /** Native g_RenderVar1: frame width, frame height, frame count, quad ratio. */
    glm::vec4 renderVar1 {0.0f};
};

inline bool valid (const Source& source) {
    if (source.mipWidth == 0 || source.mipHeight == 0 || source.authored.empty ())
        return false;
    return std::all_of (source.authored.begin (), source.authored.end (), [] (const TexelRect& rect) {
        return std::isfinite (rect.x) && std::isfinite (rect.y) && std::isfinite (rect.z)
            && std::isfinite (rect.w) && rect.z > 0.0f && rect.w > 0.0f;
    });
}

// normalizes TEXS frames by the dimensions of the page they were
// uploaded to; the first frame feeds g_RenderVar1.xy.
inline glm::vec4 nativeFrameSize (const Source& source) {
    const float frameWidth = source.authored.front ().z / static_cast<float> (source.mipWidth);
    const float frameHeight = source.authored.front ().w / static_cast<float> (source.mipHeight);
    return {frameWidth, frameHeight, 0.0f, 0.0f};
}

// g_RenderVar1.w = (mip0 height / mip0 width) * (frame height / frame width):
// the frame's pixel aspect, since the texture object takes the uploaded mip0
// dimensions rather than the TEXV header (W12 native capture, 2.8.42).
inline float nativeQuadRatio (const Source& source) {
    const glm::vec4 size = nativeFrameSize (source);
    const float pageRatio = static_cast<float> (source.mipHeight) / static_cast<float> (source.mipWidth);
    return pageRatio * (size.y / size.x);
}

// Native window of frame k: common_particles.h ComputeSpriteFrame with the
// inputs. The texture object carries the uploaded mip0 size, so the
// SPRITESHEETBLENDNPOT branch is selected iff the image is narrower than mip0
// and g_Texture0Resolution.z / .x is image width / mip0 width. float32 as on
// the GPU; written as separate steps so no step is fused.
inline glm::vec4 nativeWindow (const Source& source, uint32_t frame) {
    const glm::vec4 size = nativeFrameSize (source);
    const float current = static_cast<float> (frame);
    if (source.imageWidth < source.mipWidth) {
        const float unpaddedWidth = static_cast<float> (source.imageWidth) / static_cast<float> (source.mipWidth);
        const float scaledFrameWidth = size.x / unpaddedWidth;
        const float position = current * scaledFrameWidth;
        const float row = std::floor (position);
        const float column = position - row;
        return {column * unpaddedWidth, row * size.y, size.x, size.y};
    }
    const float position = current * size.x;
    const float row = std::floor (position);
    return {position - row, row * size.y, size.x, size.y};
}

inline float edgeDeviation (const TexelRect& a, const TexelRect& b) {
    return std::max ({std::abs (a.x - b.x), std::abs (a.y - b.y),
                      std::abs ((a.x + a.z) - (b.x + b.z)), std::abs ((a.y + a.w) - (b.y + b.w))});
}

/** True when any texel of the rectangle has nonzero coverage. */
using CoverageQuery = std::function<bool (const TexelRect&)>;

inline bool offPage (const Source& source, const TexelRect& rect) {
    return rect.x >= static_cast<float> (source.mipWidth) || rect.y >= static_cast<float> (source.mipHeight)
        || rect.x + rect.z <= 0.0f || rect.y + rect.w <= 0.0f;
}

inline bool onPage (const Source& source, const TexelRect& rect) {
    return rect.x >= -1.0f && rect.y >= -1.0f && rect.x + rect.z <= static_cast<float> (source.mipWidth) + 1.0f
        && rect.y + rect.w <= static_cast<float> (source.mipHeight) + 1.0f;
}

// Per frame, the authored TEXS rect is the reference (W12 root decision rule):
// a native window within one texel of it is used as is; a native window that
// departs from it is a native mis-index and the authored rect is kept, unless
// the authored rect samples only empty texels while the native window has
// coverage. Coverage is queried only for frames that depart.
// A TEXS layout that places any frame entirely off the uploaded page cannot
// describe that page (shipped leaves/rosepetals write one column too few for
// fractional frame widths); when native windows tile the page instead, they
// are used for every frame.
inline Layout layout (const Source& source, const CoverageQuery& coverage) {
    Layout result;
    if (!valid (source)) return result;
    const auto count = static_cast<uint32_t> (source.authored.size ());
    const glm::vec4 page (static_cast<float> (source.mipWidth), static_cast<float> (source.mipHeight),
                          static_cast<float> (source.mipWidth), static_cast<float> (source.mipHeight));
    bool authoredOffPage = false, nativeOnPage = true;
    for (uint32_t frame = 0; frame < count; ++frame) {
        authoredOffPage = authoredOffPage || offPage (source, source.authored[frame]);
        nativeOnPage = nativeOnPage && onPage (source, nativeWindow (source, frame) * page);
    }
    const bool nativeLayout = authoredOffPage && nativeOnPage;
    result.windows.reserve (count);
    result.sources.reserve (count);
    for (uint32_t frame = 0; frame < count; ++frame) {
        const glm::vec4 native = nativeWindow (source, frame);
        const TexelRect nativeTexels = native * page;
        const TexelRect& authored = source.authored[frame];
        WindowSource selected = WindowSource::Native;
        if (edgeDeviation (nativeTexels, authored) > 1.0f) {
            if (nativeLayout)
                selected = WindowSource::NativeOverOffPageLayout;
            else
                selected = coverage && !coverage (authored) && coverage (nativeTexels)
                    ? WindowSource::NativeOverEmptyAuthored : WindowSource::Authored;
        }
        result.windows.push_back (selected == WindowSource::Authored ? authored / page : native);
        result.sources.push_back (selected);
    }
    const glm::vec4 size = nativeFrameSize (source);
    result.renderVar1 = {size.x, size.y, static_cast<float> (count), nativeQuadRatio (source)};
    return result;
}

/**
 * Coverage of a texel rectangle in a tightly packed 8-bit image. Texels whose
 * area intersects the rectangle are inspected; texels outside the page are empty.
 */
inline bool rectHasCoverage (const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t channels,
                             uint32_t channel, const TexelRect& rect) {
    if (pixels == nullptr || channel >= channels) return false;
    constexpr float epsilon = 1e-3f;
    const auto clampIndex = [] (float value, uint32_t limit) {
        return static_cast<uint32_t> (std::clamp (value, 0.0f, static_cast<float> (limit)));
    };
    const uint32_t x0 = clampIndex (std::floor (rect.x + epsilon), width);
    const uint32_t x1 = clampIndex (std::ceil (rect.x + rect.z - epsilon), width);
    const uint32_t y0 = clampIndex (std::floor (rect.y + epsilon), height);
    const uint32_t y1 = clampIndex (std::ceil (rect.y + rect.w - epsilon), height);
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x)
            if (pixels[(static_cast<size_t> (y) * width + x) * channels + channel] != 0) return true;
    return false;
}

struct FrameSample {
    uint32_t current = 0;
    uint32_t next = 0;
    float blend = 0.0f;
};

// genericparticle.vert passes frac(in_ParticleLifeTime) to ComputeSpriteFrame:
// current = floor(l * N), next = min(N - 1, current + 1), blend = frac(l * N).
inline FrameSample sampleFrames (float lifetime, uint32_t count) {
    if (count == 0 || !std::isfinite (lifetime)) return {};
    const float phase = lifetime - std::floor (lifetime);
    const float frames = static_cast<float> (count);
    const float scaled = phase * frames;
    const float current = std::min (std::floor (scaled), frames - 1.0f);
    const float next = std::min (frames - 1.0f, current + 1.0f);
    return {static_cast<uint32_t> (std::max (current, 0.0f)), static_cast<uint32_t> (std::max (next, 0.0f)),
            scaled - std::floor (scaled)};
}

// Same-page atlas sequences follow in_ParticleLifeTime = age * rcp(life) *
// multiplier; TEXS frame durations do not enter the timeline.
inline float sequenceFrame (double sequenceCycles, uint32_t count, bool once) {
    const float frames = static_cast<float> (count);
    if (once) return std::min (static_cast<float> (sequenceCycles * count), frames - 1.0f);
    const auto cycle = static_cast<float> (std::fmod (sequenceCycles, 1.0));
    return std::fmod (cycle * frames, frames);
}

/**
 * Layouts shared per texture page. Every particle node on the same texture
 * (including event children created during playback) reuses one layout, so
 * the page coverage is read at most once while the texture lives. Entries
 * whose texture expired are dropped; a new texture at a recycled address is
 * never matched because its weak owner differs.
 */
template <typename Owner> class LayoutCache {
public:
    template <typename Build>
    std::shared_ptr<const Layout> get (const std::shared_ptr<const Owner>& owner, uint32_t page, Build&& build) {
        std::erase_if (m_entries, [] (const Entry& entry) { return entry.owner.expired (); });
        for (const auto& entry : m_entries)
            if (entry.page == page && entry.owner.lock () == owner) return entry.layout;
        auto layout = std::make_shared<const Layout> (build ());
        m_entries.push_back ({owner, page, layout});
        return layout;
    }
    [[nodiscard]] size_t size () const { return m_entries.size (); }

private:
    struct Entry {
        std::weak_ptr<const Owner> owner;
        uint32_t page;
        std::shared_ptr<const Layout> layout;
    };
    std::vector<Entry> m_entries;
};

} // namespace WallpaperEngine::Render::Objects::ParticleAtlas
