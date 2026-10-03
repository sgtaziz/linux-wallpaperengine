#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>

namespace WallpaperEngine::Render::Objects {
struct ShapedTextGlyph {
    FT_UInt glyphIndex;
    uint32_t cluster;
    int32_t xAdvance26_6;
    int32_t yAdvance26_6;
    int32_t xOffset26_6;
    int32_t yOffset26_6;
    /** HB_GLYPH_FLAG_UNSAFE_TO_BREAK: breaking before this glyph changes shaping. */
    bool unsafeToBreak = false;
};

struct ShapedTextRun {
    std::vector<ShapedTextGlyph> glyphs;
    hb_direction_t direction;
};

inline ShapedTextRun shapeTextRun (FT_Face face, std::span<const char32_t> codepoints) {
    ShapedTextRun result {{}, HB_DIRECTION_INVALID};
    if (!face || codepoints.empty ()) return result;
    hb_font_t* font = hb_ft_font_create_referenced (face);
    hb_buffer_t* buffer = hb_buffer_create ();
    hb_buffer_add_utf32 (buffer, reinterpret_cast<const uint32_t*> (codepoints.data ()),
                        static_cast<int> (codepoints.size ()), 0,
                        static_cast<int> (codepoints.size ()));
    hb_buffer_guess_segment_properties (buffer);
    result.direction = hb_buffer_get_direction (buffer);
    hb_shape (font, buffer, nullptr, 0);
    unsigned count = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos (buffer, &count);
    const hb_glyph_position_t* position = hb_buffer_get_glyph_positions (buffer, &count);
    result.glyphs.reserve (count);
    for (unsigned index = 0; index < count; ++index) {
        result.glyphs.push_back ({info[index].codepoint, info[index].cluster,
                                 position[index].x_advance, position[index].y_advance,
                                 position[index].x_offset, position[index].y_offset,
                                 (hb_glyph_info_get_glyph_flags (&info[index]) & HB_GLYPH_FLAG_UNSAFE_TO_BREAK) != 0});
    }
    hb_buffer_destroy (buffer);
    hb_font_destroy (font);
    return result;
}

/** Native shapes the complete contiguous font-face run,
 * including spaces, with one guessed script/direction. Splitting words loses
 * RTL word order; splitting scripts invents a different mixed-text layout.
 * This is the native first-strong-script contract, not paragraph bidi. */
inline ShapedTextRun shapeTextRow (FT_Face face, std::span<const char32_t> codepoints) {
    return shapeTextRun (face, codepoints);
}
} // namespace WallpaperEngine::Render::Objects
