#pragma once

#include <cstdint>
#include <cstddef>
#include <cmath>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace WallpaperEngine::Render::Objects {

inline uint8_t textBitmapCoverage (const FT_Bitmap& bitmap, unsigned int row, unsigned int col) {
    if (!bitmap.buffer || row >= bitmap.rows || col >= bitmap.width) return 0;
    // FreeType defines pitch as the signed pointer offset for moving down one
    // row; buffer points at the first logical row even when pitch is negative.
    const auto* source = bitmap.buffer + static_cast<std::ptrdiff_t> (row) * bitmap.pitch;
    switch (bitmap.pixel_mode) {
    case FT_PIXEL_MODE_GRAY:
        return source[col];
    case FT_PIXEL_MODE_MONO:
        return source[col / 8] & (0x80 >> (col % 8)) ? 255 : 0;
    case FT_PIXEL_MODE_BGRA:
        return source[col * 4 + 3];
    default:
        return 0;
    }
}

// Combine direct glyph draws in straight color space. The destination may
// already hold the translucent opaque-background rectangle; caller supplies
// the effective per-glyph node alpha with the bitmap coverage.
inline void compositeTextRgba (uint8_t* destination, uint8_t red, uint8_t green,
                               uint8_t blue, uint8_t alpha) {
    if (alpha == 0) return;
    const int inverse = 255 - alpha;
    const int previousAlpha = destination[3];
    const int resultAlpha = alpha + (previousAlpha * inverse + 127) / 255;
    const uint8_t source[3] = {red, green, blue};
    for (int channel = 0; channel < 3; ++channel) {
        const int previous = (destination[channel] * previousAlpha * inverse + 127) / 255;
        destination[channel] = static_cast<uint8_t> (
            (source[channel] * alpha + previous + resultAlpha / 2) / resultAlpha);
    }
    destination[3] = static_cast<uint8_t> (resultAlpha);
}

// Native translucent drawing into an RGBA effect target uses SRC_ALPHA /
// INV_SRC_ALPHA for both color and alpha. A half-alpha glyph therefore writes
// half its RGB and one quarter alpha over a cleared target.
inline void compositeTextOffscreenRgba (uint8_t* destination, uint8_t red,
                                        uint8_t green, uint8_t blue, uint8_t alpha) {
    if (alpha == 0) return;
    const int inverse = 255 - alpha;
    const uint8_t source[3] = {red, green, blue};
    for (int channel = 0; channel < 3; ++channel)
        destination[channel] = static_cast<uint8_t> (
            (source[channel] * alpha + destination[channel] * inverse + 127) / 255);
    destination[3] = static_cast<uint8_t> (
        (alpha * alpha + destination[3] * inverse + 127) / 255);
}

// The HDR glyph draw carries unclamped floating RGB before coverage and
// blending. Preserve the existing coverage/alpha quantization independently
// while allowing monochrome fill and background values to exceed one.
inline void compositeTextRgba (float* destination, float red, float green,
                               float blue, uint8_t alpha) {
    if (alpha == 0) return;
    const int inverse = 255 - alpha;
    const int previousAlpha = static_cast<int> (std::lround (destination[3] * 255.0f));
    const int resultAlpha = alpha + (previousAlpha * inverse + 127) / 255;
    const float source[3] = {red, green, blue};
    for (int channel = 0; channel < 3; ++channel)
        destination[channel] = (source[channel] * alpha
            + destination[channel] * previousAlpha * inverse / 255.0f) / resultAlpha;
    destination[3] = resultAlpha / 255.0f;
}

inline void compositeTextOffscreenRgba (float* destination, float red, float green,
                                        float blue, uint8_t alpha) {
    if (alpha == 0) return;
    const int inverse = 255 - alpha;
    const int previousAlpha = static_cast<int> (std::lround (destination[3] * 255.0f));
    const float source[3] = {red, green, blue};
    for (int channel = 0; channel < 3; ++channel)
        destination[channel] = (source[channel] * alpha + destination[channel] * inverse) / 255.0f;
    destination[3] = ((alpha * alpha + previousAlpha * inverse + 127) / 255) / 255.0f;
}

} // namespace WallpaperEngine::Render::Objects
