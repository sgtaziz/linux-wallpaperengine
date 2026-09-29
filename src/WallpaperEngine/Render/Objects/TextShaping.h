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
                                 position[index].x_offset, position[index].y_offset});
    }
    hb_buffer_destroy (buffer);
    hb_font_destroy (font);
    return result;
}

/** Shape each script word with its own guessed properties. A single buffer for
 * a mixed Latin/Arabic row inherits the first script and leaves later Arabic
 * letters disconnected. Full Unicode bidi paragraph reordering is separate. */
inline ShapedTextRun shapeTextRow (FT_Face face, std::span<const char32_t> codepoints) {
    ShapedTextRun result {{}, HB_DIRECTION_INVALID};
    if (!face || codepoints.empty ()) return result;
    hb_unicode_funcs_t* unicode = hb_unicode_funcs_get_default ();
    size_t start = 0;
    hb_script_t script = HB_SCRIPT_INVALID;
    const auto append = [&] (size_t end) {
        if (end <= start) return;
        ShapedTextRun segment = shapeTextRun (face, codepoints.subspan (start, end - start));
        if (result.direction == HB_DIRECTION_INVALID) result.direction = segment.direction;
        for (auto& glyph : segment.glyphs) {
            glyph.cluster += static_cast<uint32_t> (start);
            result.glyphs.push_back (glyph);
        }
    };
    for (size_t index = 0; index < codepoints.size (); ++index) {
        const char32_t codepoint = codepoints[index];
        if (codepoint == ' ' || codepoint == '\t') {
            append (index);
            start = index;
            append (index + 1);
            start = index + 1;
            script = HB_SCRIPT_INVALID;
            continue;
        }
        const hb_script_t current = hb_unicode_script (unicode, static_cast<hb_codepoint_t> (codepoint));
        if (current == HB_SCRIPT_COMMON || current == HB_SCRIPT_INHERITED
            || current == HB_SCRIPT_UNKNOWN) continue;
        if (script != HB_SCRIPT_INVALID && current != script) {
            append (index);
            start = index;
        }
        script = current;
    }
    append (codepoints.size ());
    return result;
}
} // namespace WallpaperEngine::Render::Objects
