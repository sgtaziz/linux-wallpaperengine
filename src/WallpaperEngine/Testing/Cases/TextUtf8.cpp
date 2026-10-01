#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "WallpaperEngine/Render/Objects/TextCodepoints.h"
#include "WallpaperEngine/Render/Objects/TextRaster.h"
#include "WallpaperEngine/Render/Objects/TextShaping.h"
#include "WallpaperEngine/Render/Objects/TextLayout.h"

#include <filesystem>
#include <algorithm>
#include <vector>

using WallpaperEngine::Render::Objects::decodeTextCodepoints;
using WallpaperEngine::Render::Objects::textBitmapCoverage;
using WallpaperEngine::Render::Objects::compositeTextRgba;
using WallpaperEngine::Render::Objects::compositeTextOffscreenRgba;
using WallpaperEngine::Render::Objects::shapeTextRun;
using WallpaperEngine::Render::Objects::shapeTextRow;
using WallpaperEngine::Render::Objects::TextLayoutRange;
using WallpaperEngine::Render::Objects::textGlyphPixelBounds;
using WallpaperEngine::Render::Objects::textRowAlignmentOffset;
using WallpaperEngine::Render::Objects::textHorizontalAnchorOffset;
using WallpaperEngine::Render::Objects::textVerticalAnchorOffset;

TEST_CASE ("Text anchors recover the raw baseline across different ink and raster extents", "[text][layout]") {
    const float ascender = 48, descender = -12, lineHeight = 60;
    // H-like ink above the baseline and g-like ink with a descender must
    // share one metric anchor; neither ink center is the native center.
    for (const auto [originY, height] : {std::pair {-36.0f, 36.0f}, std::pair {-27.0f, 38.0f},
                                       std::pair {-48.0f, 120.0f}}) {
        const float rasterCenterUp = -(originY + height * .5f);
        for (int rows : {1, 2}) {
            const float preceding = (rows - 1) * lineHeight;
            for (const auto alignment : {"top", "center", "bottom"}) {
                const float offset = textVerticalAnchorOffset (rasterCenterUp, ascender, descender,
                                                               rows, lineHeight, alignment);
                const float baselineInCenteredRaster = -rasterCenterUp;
                const float expected = alignment == std::string_view ("top") ? -ascender
                    : alignment == std::string_view ("bottom") ? -descender + preceding
                    : (-ascender + preceding) * .5f;
                REQUIRE (baselineInCenteredRaster + offset == expected);
                // Moving to another row retains the native negative-up
                // line advance, including overlapping/reverse-order rows.
                REQUIRE (baselineInCenteredRaster - lineHeight + offset == expected - lineHeight);
            }
        }
    }
    REQUIRE (textVerticalAnchorOffset (0, 48, -12, 3, -10, "center") == -34);
}

TEST_CASE ("Unequal text rows align their positioned glyph bounds independently of advance", "[text][layout]") {
    const TextLayoutRange wide {-2, 78}, narrow {0, 26};
    for (const auto alignment : {"left", "center", "right"}) {
        const float wideShift = textRowAlignmentOffset (wide.width (), wide.width (), alignment);
        const float narrowShift = textRowAlignmentOffset (narrow.width (), wide.width (), alignment);
        REQUIRE (wideShift == 0);
        REQUIRE (narrowShift == (alignment == std::string_view ("left") ? 0.0f
            : alignment == std::string_view ("right") ? 54.0f : 27.0f));
        TextLayoutRange layout;
        layout.include (wide.min, wide.max);
        layout.include (narrow.min, narrow.max);
        REQUIRE (layout.width () == 80);
        if (alignment == std::string_view ("right")) {
            // Recomputing global bounds after moving rows would incorrectly
            // expand this layout to 82 and shift the right anchor by two.
            REQUIRE (narrow.max + narrowShift > layout.max);
        }
        // Raster widths may retain extra logical space after the ink. The
        // native positioned glyph anchor must not move with that margin.
        for (const float rasterCenter : {40.0f, 50.0f}) {
            const float offset = textHorizontalAnchorOffset (rasterCenter, layout, alignment);
            const float rawGlyph = 5.0f;
            const float expected = alignment == std::string_view ("left") ? 7.0f
                : alignment == std::string_view ("right") ? -73.0f : -33.0f;
            REQUIRE (rawGlyph - rasterCenter + offset == expected);
        }
    }
}

TEST_CASE ("FreeType text layout uses pixel glyph boxes without the final pen advance", "[text][layout]") {
    const char* font = nullptr;
    for (const auto* candidate : {"/usr/share/fonts/TTF/DejaVuSans.ttf",
                                  "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}) {
        if (std::filesystem::exists (candidate)) { font = candidate; break; }
    }
    REQUIRE (font != nullptr);
    FT_Library library = nullptr;
    REQUIRE (FT_Init_FreeType (&library) == 0);
    FT_Face face = nullptr;
    REQUIRE (FT_New_Face (library, font, 0, &face) == 0);
    REQUIRE (FT_Set_Char_Size (face, 0, 12 * 64, 300, 300) == 0);
    const std::vector<char32_t> row {'H', 'H', 'H'};
    const auto shaped = shapeTextRun (face, row);
    TextLayoutRange bounds;
    float advance = 0;
    for (const auto& glyph : shaped.glyphs) {
        FT_BBox box {};
        REQUIRE (textGlyphPixelBounds (face, glyph.glyphIndex, box));
        bounds.include (advance + (glyph.xOffset26_6 >> 6) + box.xMin,
                        advance + (glyph.xOffset26_6 >> 6) + box.xMax);
        advance += glyph.xAdvance26_6 >> 6;
    }
    REQUIRE (bounds.min == 0);
    REQUIRE (bounds.max > 0);
    REQUIRE (bounds.max < advance);
    FT_BBox descender {};
    REQUIRE (textGlyphPixelBounds (face, FT_Get_Char_Index (face, 'g'), descender));
    REQUIRE (descender.yMin < 0);
    REQUIRE (descender.yMax > 0);
    FT_BBox negativeBearing {};
    REQUIRE (textGlyphPixelBounds (face, FT_Get_Char_Index (face, 'j'), negativeBearing));
    REQUIRE (negativeBearing.xMin < 0);
    FT_Done_Face (face);
    FT_Done_FreeType (library);
}

TEST_CASE ("HDR monochrome RGB survives coverage while brightness leaves alpha independent", "[text][raster][hdr]") {
    float bright[] {0, 0, 0, 0}, dark[] {0, 0, 0, 0};
    compositeTextRgba (bright, 2.0f, 2.0f, 2.0f, 128);
    compositeTextRgba (dark, 0.0f, 0.0f, 0.0f, 128);
    REQUIRE (bright[0] == 2.0f);
    REQUIRE (bright[3] == dark[3]);
    // Native >1 evidence is carried by antialiased edges even when the
    // presentation saturates the fully covered glyph core.
    REQUIRE (bright[0] * bright[3] == Catch::Approx (256.0f / 255.0f));
    float offscreen[] {0, 0, 0, 0}, unit[] {0, 0, 0, 0};
    compositeTextOffscreenRgba (offscreen, 2.0f, .5f, .125f, 128);
    compositeTextOffscreenRgba (unit, 1.0f, .5f, .125f, 128);
    REQUIRE (offscreen[0] > 1.0f);
    REQUIRE (offscreen[3] == unit[3]);
    REQUIRE (offscreen[3] == 64.0f / 255.0f);
    // A subsequent color-font draw consumes atlas RGB, independently of
    // the earlier monochrome fill's brightness and without a final multiply.
    compositeTextRgba (bright, .8f, .3f, .1f, 255);
    REQUIRE (bright[0] == .8f);
    REQUIRE (bright[1] == .3f);
    REQUIRE (bright[2] == .1f);
}

TEST_CASE ("HDR background and glyph contributions compose before backdrop sampling", "[text][raster][hdr]") {
    float background[] {.5f, .5f, .5f, 1.0f};
    compositeTextRgba (background, 0.0f, 0.0f, 0.0f, 128);
    REQUIRE (background[0] == Catch::Approx (.5f * 127.0f / 255.0f));
    REQUIRE (background[3] == 1.0f);
    float glyph[] {0, 0, 0, 0};
    compositeTextOffscreenRgba (glyph, 2.0f, 2.0f, 2.0f, 128);
    const float backdrop = .3f, remaining = 127.0f / 255.0f;
    REQUIRE (glyph[0] + backdrop * remaining == Catch::Approx (256.0f / 255.0f + backdrop * remaining));
}

TEST_CASE ("HDR text gain-down retains bright target RGB and independent translucent alpha", "[text][raster][hdr][effect]") {
    // Native positive: brightness4 * authored.5 enters the effect as RGB2;
    // gain.25 then matches a unit-brightness .5 glyph. Clamping the retained
    // target to one produces .25 instead and loses this equality.
    for (uint8_t coverage : {uint8_t (128), uint8_t (255)}) {
        float bright[] {0, 0, 0, 0}, control[] {0, 0, 0, 0};
        compositeTextOffscreenRgba (bright, 2.0f, 2.0f, 2.0f, coverage);
        compositeTextOffscreenRgba (control, .5f, .5f, .5f, coverage);
        REQUIRE (bright[0] * .25f == control[0]);
        REQUIRE (bright[3] == control[3]);
        REQUIRE (std::min (bright[0], 1.0f) * .25f < control[0]);
    }
}

TEST_CASE ("Scene text decodes complete Unicode scalar values", "[text][utf8]") {
    const auto codepoints = decodeTextCodepoints ("A\xc3\xa9\xce\xa9\xf0\x9f\x99\x82");
    const std::vector<char32_t> expected {'A', 0x00e9, 0x03a9, 0x1f642};
    REQUIRE (codepoints == expected);
}

TEST_CASE ("Malformed UTF-8 cannot turn into unrelated glyph indexes", "[text][utf8]") {
    const std::vector<char32_t> replacement {0xfffd};
    const std::vector<char32_t> replacementThenA {0xfffd, 'A'};
    REQUIRE (decodeTextCodepoints ("\xed\xa0\x80") == replacement);
    REQUIRE (decodeTextCodepoints ("\xe2" "A") == replacementThenA);
    REQUIRE (decodeTextCodepoints ("\xf4\x90\x80\x80") == replacement);
}

TEST_CASE ("Text raster respects FreeType's signed row pitch", "[text][raster]") {
    unsigned char storage[] {9, 10, 11, 12};
    FT_Bitmap bitmap {};
    bitmap.buffer = storage + 2;
    bitmap.width = 2;
    bitmap.rows = 2;
    bitmap.pitch = -2;
    bitmap.pixel_mode = FT_PIXEL_MODE_GRAY;

    REQUIRE (textBitmapCoverage (bitmap, 0, 0) == 11);
    REQUIRE (textBitmapCoverage (bitmap, 0, 1) == 12);
    REQUIRE (textBitmapCoverage (bitmap, 1, 0) == 9);
    REQUIRE (textBitmapCoverage (bitmap, 1, 1) == 10);
    REQUIRE (textBitmapCoverage (bitmap, 2, 0) == 0);
}

TEST_CASE ("Overlapping glyph coverage composes in draw order", "[text][raster]") {
    uint8_t pixel[] {255, 255, 255, 0};
    compositeTextRgba (pixel, 255, 255, 255, 128);
    compositeTextRgba (pixel, 255, 255, 255, 128);
    REQUIRE (pixel[3] == 192);
    REQUIRE (pixel[0] == 255);
    REQUIRE (pixel[1] == 255);
    REQUIRE (pixel[2] == 255);

    compositeTextRgba (pixel, 255, 0, 0, 255);
    REQUIRE (pixel[3] == 255);
    REQUIRE (pixel[0] == 255);
    REQUIRE (pixel[1] == 0);
    REQUIRE (pixel[2] == 0);
}

TEST_CASE ("Native offscreen translucent glyphs square target alpha", "[text][raster]") {
    uint8_t pixel[] {0, 0, 0, 0};
    compositeTextOffscreenRgba (pixel, 255, 172, 51, 128);
    REQUIRE (pixel[0] == 128);
    REQUIRE (pixel[1] == 86);
    REQUIRE (pixel[2] == 26);
    REQUIRE (pixel[3] == 64);
    compositeTextOffscreenRgba (pixel, 255, 172, 51, 128);
    REQUIRE (pixel[0] == 192);
    REQUIRE (pixel[3] == 96);
}

TEST_CASE ("HarfBuzz shapes Latin ligature and Arabic order with the selected FreeType face", "[text][shaping]") {
    const char* font = nullptr;
    for (const auto* candidate : {"/usr/share/fonts/TTF/DejaVuSans.ttf",
                                  "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}) {
        if (std::filesystem::exists (candidate)) {
            font = candidate;
            break;
        }
    }
    REQUIRE (font != nullptr);
    FT_Library library = nullptr;
    REQUIRE (FT_Init_FreeType (&library) == 0);
    FT_Face face = nullptr;
    REQUIRE (FT_New_Face (library, font, 0, &face) == 0);
    REQUIRE (FT_Set_Char_Size (face, 0, 12 * 64, 300, 300) == 0);

    const std::vector<char32_t> latin {'f', 'i'};
    const auto fi = shapeTextRun (face, latin);
    REQUIRE (fi.direction == HB_DIRECTION_LTR);
    REQUIRE (fi.glyphs.size () == 1);
    REQUIRE (fi.glyphs[0].glyphIndex != FT_Get_Char_Index (face, 'f'));
    REQUIRE (fi.glyphs[0].xAdvance26_6 > 0);

    const std::vector<char32_t> arabic {0x0633, 0x0644, 0x0627, 0x0645};
    const auto salaam = shapeTextRun (face, arabic);
    REQUIRE (salaam.direction == HB_DIRECTION_RTL);
    REQUIRE (salaam.glyphs.size () < arabic.size ());
    REQUIRE (salaam.glyphs[0].cluster == 3);

    const std::vector<char32_t> mixed {'A', 'V', ' ', 0x0633, 0x0644, 0x0627, 0x0645,
                                       ' ', 'f', 'i'};
    const auto mixedRow = shapeTextRow (face, mixed);
    REQUIRE (mixedRow.glyphs.size () == 8);
    REQUIRE (mixedRow.glyphs[7].glyphIndex == fi.glyphs[0].glyphIndex);
    REQUIRE (mixedRow.glyphs[3].glyphIndex == salaam.glyphs[0].glyphIndex);
    REQUIRE (mixedRow.glyphs[3].cluster == 6);

    FT_Done_Face (face);
    FT_Done_FreeType (library);
}
