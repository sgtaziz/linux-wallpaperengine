#pragma once

#include <algorithm>
#include <string_view>

#include "TextLayoutBox.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H

namespace WallpaperEngine::Render::Objects {
// Native rows include the baseline origin in their positioned glyph bounds.
// Advance places the following glyph; it is not part of the final row width.
struct TextLayoutRange {
    float min = 0.0f;
    float max = 0.0f;
    void include (float low, float high) {
        min = std::min (min, low);
        max = std::max (max, high);
    }
    [[nodiscard]] float width () const { return max - min; }
};

inline bool textGlyphPixelBounds (FT_Face face, FT_UInt index, FT_BBox& bounds) {
    if (FT_Load_Glyph (face, index, FT_LOAD_NO_BITMAP) != 0) return false;
    FT_Glyph glyph = nullptr;
    if (FT_Get_Glyph (face->glyph, &glyph) != 0) return false;
    FT_Glyph_Get_CBox (glyph, FT_GLYPH_BBOX_PIXELS, &bounds);
    FT_Done_Glyph (glyph);
    return true;
}

inline float textRowAlignmentOffset (float rowWidth, float maxRowWidth,
                                     std::string_view alignment) {
    if (alignment == "left") return 0.0f;
    const float remaining = maxRowWidth - rowWidth;
    return alignment == "right" ? remaining : remaining * 0.5f;
}

inline float textHorizontalAnchorOffset (float rasterCenter, const TextLayoutRange& bounds,
                                         std::string_view alignment) {
    const float anchor = alignment == "left" ? 0.0f
        : alignment == "right" ? bounds.width () : bounds.width () * 0.5f;
    return rasterCenter - anchor - std::min (0.0f, bounds.min);
}

/** `horizontal` holds the positioned glyph ink with the origin included;
 * `lowestInk` / `highestInk` are the row ink bottom / top extremes, each row's
 * taken with its baseline (0) and offset by row × -lineHeight. Every row, the
 * last included, takes one full line height below the ascender; ink beyond
 * that line box grows it on either side. */
inline TextLayoutBox textLayoutBoxFromBounds (const TextLayoutRange& horizontal, float lowestInk,
                                              float highestInk, float ascender, float lineHeight,
                                              int rows) {
    return {horizontal.min, horizontal.max,
            std::min (ascender - static_cast<float> (rows) * lineHeight, lowestInk),
            std::max (ascender, highestInk)};
}

/** Native: the background/effect margin around the layout box.
 * Zero unless the text has visible effects or an opaque background; each
 * axis is capped at 512 with no lower clamp. */
inline float textLayerPadding (float authored, bool effects, bool opaqueBackground) {
    if (!effects && !opaqueBackground) return 0.0f;
    return std::min (authored, 512.0f);
}

inline float textVerticalAnchorOffset (float rasterCenterUp, float ascender, float descender,
                                       int rows, float lineHeight, std::string_view alignment) {
    const float precedingRows = static_cast<float> (rows - 1) * lineHeight;
    const float anchor = alignment == "top" ? ascender
        : alignment == "bottom" ? descender - precedingRows
        : (ascender - precedingRows) * 0.5f;
    return rasterCenterUp - anchor;
}
} // namespace WallpaperEngine::Render::Objects
