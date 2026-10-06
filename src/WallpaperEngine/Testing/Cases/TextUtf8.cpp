#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "WallpaperEngine/Render/Objects/TextCodepoints.h"
#include "WallpaperEngine/Render/Objects/TextRaster.h"
#include "WallpaperEngine/Render/Objects/TextShaping.h"
#include "WallpaperEngine/Render/Objects/TextLayout.h"
#include "WallpaperEngine/Render/Objects/TextRowLayout.h"

#include <cstdlib>
#include <filesystem>
#include <string_view>
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

    // Native full-face-run controls: a space must not split the RTL words.
    const auto twoWords = decodeTextCodepoints ("مرحبا بالعالم");
    const auto rtlRow = shapeTextRow (face, twoWords);
    REQUIRE (rtlRow.direction == HB_DIRECTION_RTL);
    REQUIRE (rtlRow.glyphs.size () == twoWords.size ());
    REQUIRE (rtlRow.glyphs.front ().cluster == twoWords.size () - 1);
    REQUIRE (rtlRow.glyphs.back ().cluster == 0);
    REQUIRE (std::is_sorted (rtlRow.glyphs.begin (), rtlRow.glyphs.end (),
        [] (const auto& left, const auto& right) { return left.cluster > right.cluster; }));

    // Native mixed text guesses the first strong script once. It does not
    // apply paragraph bidi or independently reconnect each later script.
    const auto latinFirst = decodeTextCodepoints ("ABC مرحبا XYZ بالعالم");
    const auto ltrMixed = shapeTextRow (face, latinFirst);
    REQUIRE (ltrMixed.direction == HB_DIRECTION_LTR);
    REQUIRE (ltrMixed.glyphs.front ().glyphIndex == FT_Get_Char_Index (face, 'A'));
    REQUIRE (ltrMixed.glyphs[4].cluster == 4);
    REQUIRE (ltrMixed.glyphs[4].glyphIndex == FT_Get_Char_Index (face, 0x0645));
    REQUIRE (ltrMixed.glyphs.back ().cluster == latinFirst.size () - 1);

    const auto arabicFirst = decodeTextCodepoints ("مرحبا ABC بالعالم 123");
    const auto rtlMixed = shapeTextRow (face, arabicFirst);
    REQUIRE (rtlMixed.direction == HB_DIRECTION_RTL);
    REQUIRE (rtlMixed.glyphs.front ().glyphIndex == FT_Get_Char_Index (face, '3'));
    REQUIRE (rtlMixed.glyphs[1].glyphIndex == FT_Get_Char_Index (face, '2'));
    REQUIRE (rtlMixed.glyphs[2].glyphIndex == FT_Get_Char_Index (face, '1'));
    REQUIRE (rtlMixed.glyphs[12].glyphIndex == FT_Get_Char_Index (face, 'C'));
    REQUIRE (rtlMixed.glyphs.back ().cluster == 0);

    const auto combining = decodeTextCodepoints ("Café Ä ñ");
    const auto accents = shapeTextRow (face, combining);
    REQUIRE (accents.glyphs.size () == 8);
    REQUIRE (accents.glyphs[3].glyphIndex == FT_Get_Char_Index (face, 0x00e9));
    REQUIRE (accents.glyphs[5].glyphIndex == FT_Get_Char_Index (face, 0x00c4));
    REQUIRE (accents.glyphs[7].glyphIndex == FT_Get_Char_Index (face, 0x00f1));
    REQUIRE (accents.glyphs[3].cluster == 3);
    REQUIRE (accents.glyphs[5].cluster == 6);
    REQUIRE (accents.glyphs[7].cluster == 9);

    FT_Done_Face (face);
    FT_Done_FreeType (library);
}

namespace {
using WallpaperEngine::Render::Objects::layoutTextRows;
using WallpaperEngine::Render::Objects::shapeNativeTextRow;
using WallpaperEngine::Render::Objects::TextFaceResolver;
using WallpaperEngine::Render::Objects::TextLaidRow;
using WallpaperEngine::Render::Objects::TextLayoutLimits;
using WallpaperEngine::Render::Objects::TextRowGlyph;
using WallpaperEngine::Render::Objects::textGlyphHorizontalInk;
using WallpaperEngine::Render::Objects::TextLayoutBox;
using WallpaperEngine::Render::Objects::textLayoutBox;
using WallpaperEngine::Render::Objects::textLayerPadding;

constexpr long kNativeTestSize26_6 = 24 * 64;

struct TestFonts {
    FT_Library library = nullptr;
    std::vector<FT_Face> faces;
    TestFonts () { REQUIRE (FT_Init_FreeType (&library) == 0); }
    ~TestFonts () {
        for (FT_Face face : faces) FT_Done_Face (face);
        FT_Done_FreeType (library);
    }
    TestFonts (const TestFonts&) = delete;
    TestFonts& operator= (const TestFonts&) = delete;
    FT_Face load (const std::filesystem::path& path) {
        FT_Face face = nullptr;
        REQUIRE (FT_New_Face (library, path.c_str (), 0, &face) == 0);
        REQUIRE (FT_Set_Char_Size (face, 0, kNativeTestSize26_6, 300, 300) == 0);
        faces.push_back (face);
        return face;
    }
    FT_Face loadSystem (const char* name) {
        for (const char* directory : {"/usr/share/fonts/TTF/", "/usr/share/fonts/truetype/dejavu/",
                                      "/usr/share/fonts/dejavu/"})
            if (std::filesystem::exists (std::filesystem::path (directory) / name))
                return load (std::filesystem::path (directory) / name);
        FAIL ("system test font is unavailable: " << name);
        return nullptr;
    }
};

// The product resolver contract: the first face covering the scalar, or
// nullptr when every face is exhausted.
TextFaceResolver resolverFor (std::vector<FT_Face> order) {
    return [order] (char32_t codepoint) -> FT_Face {
        for (FT_Face face : order)
            if (FT_Get_Char_Index (face, static_cast<FT_ULong> (codepoint)) != 0) return face;
        return nullptr;
    };
}

std::vector<char32_t> scalars (std::string_view utf8) { return decodeTextCodepoints (utf8); }

std::vector<std::vector<char32_t>> rowTexts (const std::vector<TextLaidRow>& rows) {
    std::vector<std::vector<char32_t>> result;
    for (const auto& row : rows) result.push_back (row.text);
    return result;
}

struct PlacedRow { std::vector<float> x; std::vector<float> right; float pen = 0.0f; };

PlacedRow placeRow (const std::vector<TextRowGlyph>& glyphs) {
    PlacedRow placed;
    for (const auto& glyph : glyphs) {
        const float x = placed.pen + static_cast<float> (glyph.glyph.xOffset26_6 >> 6);
        float low = 0.0f, high = 0.0f;
        REQUIRE (textGlyphHorizontalInk (glyph.face, glyph.glyph.glyphIndex, low, high));
        placed.x.push_back (x);
        placed.right.push_back (x + high);
        placed.pen += static_cast<float> (glyph.glyph.xAdvance26_6 >> 6);
    }
    return placed;
}

// Ink right edge of the nth glyph that advances the pen; zero-width
// default-ignorables do not count as a visible column.
float inkRightOfAdvancingGlyph (const std::vector<TextRowGlyph>& glyphs, size_t nth) {
    const auto placed = placeRow (glyphs);
    size_t seen = 0;
    for (size_t index = 0; index < glyphs.size (); ++index)
        if ((glyphs[index].glyph.xAdvance26_6 >> 6) != 0 && ++seen == nth) return placed.right[index];
    FAIL ("row has too few advancing glyphs");
    return 0.0f;
}

TextLayoutLimits widthLimit (float maxWidth, int maxRows = 0, bool ellipsis = false) {
    TextLayoutLimits limits;
    limits.maxWidth = maxWidth;
    limits.maxRows = maxRows;
    limits.ellipsis = ellipsis;
    return limits;
}

TextLayoutLimits rowLimit (int maxRows, bool ellipsis) {
    TextLayoutLimits limits;
    limits.maxRows = maxRows;
    limits.ellipsis = ellipsis;
    return limits;
}

using Rows = std::vector<std::vector<char32_t>>;
} // namespace

TEST_CASE ("Native wrap triggers on the ink right edge rather than the advance", "[text][layout][wrap]") {
    TestFonts fonts;
    const FT_Face sans = fonts.loadSystem ("DejaVuSans.ttf");
    const auto resolve = resolverFor ({sans});

    const auto dotted = scalars ("aaaaaaa.");
    const auto dottedPlaced = placeRow (shapeNativeTextRow (dotted, resolve).glyphs);
    const float dotInkRight = dottedPlaced.right.back ();
    // The period's ink ends well before its advance, which the old wrap measured.
    REQUIRE (dotInkRight < dottedPlaced.pen);
    auto rows = layoutTextRows (dotted, resolve, widthLimit (dotInkRight));
    REQUIRE (rows.size () == 1);
    REQUIRE (rows[0].glyphs.size () == 8);
    rows = layoutTextRows (dotted, resolve, widthLimit (dotInkRight - 1.0f));
    REQUIRE (rowTexts (rows) == Rows {scalars ("aaaaaaa"), scalars (".")});

    const auto eight = scalars ("aaaaaaaa");
    const float eighthInk = placeRow (shapeNativeTextRow (eight, resolve).glyphs).right.back ();
    rows = layoutTextRows (eight, resolve, widthLimit (eighthInk - 1.0f));
    REQUIRE (rowTexts (rows) == Rows {scalars ("aaaaaaa"), scalars ("a")});
    REQUIRE (rows[0].glyphs.size () == 7);
    REQUIRE (rows[1].glyphs.size () == 1);
}

TEST_CASE ("Only ASCII space, tab and CR are native wrap opportunities", "[text][layout][wrap]") {
    TestFonts fonts;
    const FT_Face sans = fonts.loadSystem ("DejaVuSans.ttf");
    const auto resolve = resolverFor ({sans});
    struct Case { const char* text; const char* first; const char* second; };
    // Native break-opportunities fixture: the seventh column fits, the eighth overflows.
    for (const Case& value : {Case {"aaaa bbbb", "aaaa", "bbbb"}, Case {"aaaa\tbbbb", "aaaa", "bbbb"},
                              Case {"aaaa-bbbb", "aaaa-bb", "bb"},
                              Case {"aaaa bbbb", "aaaa bb", "bb"},
                              Case {"aaaa​bbbb", "aaaa​bbb", "b"}}) {
        INFO (value.text);
        const auto text = scalars (value.text);
        const float maxWidth = inkRightOfAdvancingGlyph (shapeNativeTextRow (text, resolve).glyphs, 7);
        const auto rows = layoutTextRows (text, resolve, widthLimit (maxWidth));
        REQUIRE (rowTexts (rows) == Rows {scalars (value.first), scalars (value.second)});
    }
}

TEST_CASE ("A native wrap at dropped leading whitespace leaves an empty first row", "[text][layout][wrap]") {
    TestFonts fonts;
    const FT_Face sans = fonts.loadSystem ("DejaVuSans.ttf");
    const auto resolve = resolverFor ({sans});
    const auto control = scalars ("MMMMMMMMMMMM");
    const float maxWidth = inkRightOfAdvancingGlyph (shapeNativeTextRow (control, resolve).glyphs, 7);

    auto rows = layoutTextRows (control, resolve, widthLimit (maxWidth));
    REQUIRE (rowTexts (rows) == Rows {scalars ("MMMMMMM"), scalars ("MMMMM")});

    rows = layoutTextRows (scalars ("  MMMMMMMMMMMM"), resolve, widthLimit (maxWidth));
    REQUIRE (rows.size () == 3);
    REQUIRE (rows[0].text.empty ());
    REQUIRE (rows[0].glyphs.empty ());
    REQUIRE (rows[1].text == scalars ("MMMMMMM"));
    REQUIRE (rows[1].glyphs.size () == 7);
    REQUIRE (rows[2].text == scalars ("MMMMM"));
    REQUIRE (rows[2].glyphs.size () == 5);
}

TEST_CASE ("The last allowed row is cut at the overflowing glyph or refitted with an ellipsis", "[text][layout][maxrows]") {
    TestFonts fonts;
    const FT_Face sans = fonts.loadSystem ("DejaVuSans.ttf");
    const auto resolve = resolverFor ({sans});
    const auto text = scalars ("aaaa bbbb cccc");
    const float sevenColumns = inkRightOfAdvancingGlyph (shapeNativeTextRow (text, resolve).glyphs, 7);

    // No back-search to the space and no trim: the glyphs kept so far stay.
    auto rows = layoutTextRows (text, resolve, widthLimit (sevenColumns, 1));
    REQUIRE (rowTexts (rows) == Rows {scalars ("aaaa bb")});
    REQUIRE (rows[0].glyphs.size () == 7);

    const auto fitted = scalars ("aaaa b…");
    const float fittedRight = placeRow (shapeNativeTextRow (fitted, resolve).glyphs).right.back ();
    rows = layoutTextRows (text, resolve, widthLimit (fittedRight, 1, true));
    REQUIRE (rowTexts (rows) == Rows {fitted});
    REQUIRE (rows[0].glyphs.size () == 7);
    REQUIRE (rows[0].glyphs.back ().glyph.glyphIndex == FT_Get_Char_Index (sans, 0x2026));
    REQUIRE (placeRow (rows[0].glyphs).right.back () <= fittedRight);

    // When only "aaaa …" fits, the refit trims the space before the ellipsis.
    const float shortRight =
        placeRow (shapeNativeTextRow (scalars ("aaaa …"), resolve).glyphs).right.back ();
    rows = layoutTextRows (text, resolve, widthLimit (shortRight, 1, true));
    REQUIRE (rowTexts (rows) == Rows {scalars ("aaaa…")});
}

TEST_CASE ("The explicit row cap appends an ellipsis before width layout refits it", "[text][layout][maxrows]") {
    TestFonts fonts;
    const FT_Face sans = fonts.loadSystem ("DejaVuSans.ttf");
    const auto resolve = resolverFor ({sans});

    auto rows = layoutTextRows (scalars ("aaaa\nbbbb \ncccc"), resolve, rowLimit (2, true));
    REQUIRE (rowTexts (rows) == Rows {scalars ("aaaa"), scalars ("bbbb…")});
    rows = layoutTextRows (scalars ("aaaa\nbbbb \ncccc"), resolve, rowLimit (2, false));
    REQUIRE (rowTexts (rows) == Rows {scalars ("aaaa"), scalars ("bbbb")});

    const auto refit = scalars ("bbbbbb…");
    const float refitRight = placeRow (shapeNativeTextRow (refit, resolve).glyphs).right.back ();
    rows = layoutTextRows (scalars ("aaaa\nbbbbbbbbbb\ncccc"), resolve, widthLimit (refitRight, 2, true));
    REQUIRE (rowTexts (rows) == Rows {scalars ("aaaa"), refit});
    REQUIRE (rows[1].glyphs.size () == 7);

    // A wrap that pushes rows past the cap trims and ellipsizes the last kept row.
    const auto wrapped = scalars ("aaaa bbbb\ncccc");
    const float sevenColumns = inkRightOfAdvancingGlyph (shapeNativeTextRow (wrapped, resolve).glyphs, 7);
    rows = layoutTextRows (wrapped, resolve, widthLimit (sevenColumns, 2, true));
    REQUIRE (rowTexts (rows) == Rows {scalars ("aaaa"), scalars ("bbbb…")});

    // A terminal LF adds no row; empty explicit rows still count.
    rows = layoutTextRows (scalars ("a\n\nb\n"), resolve, TextLayoutLimits {});
    REQUIRE (rowTexts (rows) == Rows {scalars ("a"), scalars (""), scalars ("b")});
}

TEST_CASE ("Glyph id 0 and leading whitespace take no native position or advance", "[text][layout][runs]") {
    TestFonts fonts;
    const FT_Face sans = fonts.loadSystem ("DejaVuSans.ttf");
    REQUIRE (FT_Get_Char_Index (sans, 0x0378) == 0);
    REQUIRE (FT_Get_Char_Index (sans, '\t') == 0);
    const auto resolve = resolverFor ({sans});
    const TextLayoutLimits none;

    const auto inner = layoutTextRows (scalars ("A͸B"), resolve, none);
    REQUIRE (inner.size () == 1);
    REQUIRE (inner[0].glyphs.size () == 2);
    REQUIRE (inner[0].glyphs[1].glyph.glyphIndex == FT_Get_Char_Index (sans, 'B'));
    REQUIRE (placeRow (inner[0].glyphs).x[1]
             == static_cast<float> (inner[0].glyphs[0].glyph.xAdvance26_6 >> 6));

    for (const char* leading : {"͸A", "    A", "\tA", "‍A"}) {
        INFO (leading);
        const auto rows = layoutTextRows (scalars (leading), resolve, none);
        REQUIRE (rows.size () == 1);
        REQUIRE (rows[0].glyphs.size () == 1);
        REQUIRE (rows[0].glyphs[0].glyph.glyphIndex == FT_Get_Char_Index (sans, 'A'));
        REQUIRE (placeRow (rows[0].glyphs).x[0] == 0.0f);
    }

    const auto trailing = layoutTextRows (scalars ("A    "), resolve, none);
    REQUIRE (trailing[0].glyphs.size () == 5);
    const auto exhausted = layoutTextRows (scalars ("͸͸\nA"), resolve, none);
    REQUIRE (exhausted.size () == 2);
    REQUIRE (exhausted[0].glyphs.empty ());
    REQUIRE (exhausted[1].glyphs.size () == 1);
}

TEST_CASE ("Whitespace and exhausted scalars stay in the current native face run", "[text][layout][runs]") {
    TestFonts fonts;
    const FT_Face sans = fonts.loadSystem ("DejaVuSans.ttf");
    const FT_Face mono = fonts.loadSystem ("DejaVuSansMono.ttf");
    const auto numeralsMono = [&] (char32_t codepoint) -> FT_Face {
        if (codepoint >= '0' && codepoint <= '9') return mono;
        return FT_Get_Char_Index (sans, codepoint) ? sans : nullptr;
    };

    const auto numerals = shapeNativeTextRow (scalars ("12 34"), numeralsMono);
    REQUIRE (numerals.glyphs.size () == 5);
    for (const auto& glyph : numerals.glyphs) {
        REQUIRE (glyph.face == mono);
        REQUIRE (glyph.runStart == 0);
    }
    REQUIRE (numerals.glyphs[2].glyph.xAdvance26_6
             == shapeTextRun (mono, scalars (" ")).glyphs[0].xAdvance26_6);

    const auto mixed = shapeNativeTextRow (scalars ("1 A"), numeralsMono);
    REQUIRE (mixed.glyphs.size () == 3);
    REQUIRE (mixed.glyphs[1].face == mono);
    REQUIRE (mixed.glyphs[2].face == sans);
    REQUIRE (mixed.glyphs[2].runStart == 2);

    // An exhausted combining mark is shaped with the base run's face, so
    // HarfBuzz composes it with its base exactly like the precomposed scalar.
    const auto markExhausted = [&] (char32_t codepoint) -> FT_Face {
        return codepoint == 0x0308 ? nullptr : sans;
    };
    const auto composed = shapeNativeTextRow (scalars ("Ä"), markExhausted);
    REQUIRE (composed.glyphs.size () == 1);
    REQUIRE (composed.glyphs[0].glyph.glyphIndex == FT_Get_Char_Index (sans, 0x00c4));
}

TEST_CASE ("Rows containing an RTL run keep the readable Linux word wrap", "[text][layout][wrap][rtl]") {
    TestFonts fonts;
    const FT_Face sans = fonts.loadSystem ("DejaVuSans.ttf");
    const auto resolve = resolverFor ({sans});
    float widestWord = 0.0f;
    for (const char* word : {"مرحبا", "بالعالم"}) {
        float width = 0.0f;
        for (const auto& glyph : shapeTextRun (sans, scalars (word)).glyphs)
            width += static_cast<float> (glyph.xAdvance26_6 >> 6);
        widestWord = std::max (widestWord, width);
    }
    const auto rows = layoutTextRows (scalars ("مرحبا بالعالم مرحبا"), resolve,
                                      widthLimit (widestWord + 1.0f, 3));
    REQUIRE (rowTexts (rows) == Rows {scalars ("مرحبا"), scalars ("بالعالم"), scalars ("مرحبا")});
    for (const auto& row : rows) REQUIRE (row.glyphs.size () == row.text.size ());
}

TEST_CASE ("Shipped fonts keep emoji spacing and reproduce native layout fixtures", "[text][layout][runs][shipped]") {
    const char* root = std::getenv ("WPE_SHIPPED_ASSETS");
    if (!root) SKIP ("WPE_SHIPPED_ASSETS is not set; shipped NotoSans/RobotoMono/Twemoji fixtures not run");
    const std::filesystem::path assets (root);
    TestFonts fonts;
    const FT_Face noto = fonts.load (assets / "fonts/NotoSans-Regular.ttf");
    const FT_Face roboto = fonts.load (assets / "fonts/RobotoMono-Regular.ttf");
    const FT_Face twemoji = fonts.load (assets / "fonts/TwemojiMozilla.ttf");
    REQUIRE (FT_Get_Char_Index (twemoji, ' ') == 0);
    const auto resolve = resolverFor ({noto, twemoji});
    const TextLayoutLimits none;

    // Linux draws colour Twemoji, which has no space: the space keeps the
    // advance of the face that supplies it instead of collapsing the gap.
    const auto pair = layoutTextRows (scalars ("😀 😀"), resolve, none);
    REQUIRE (pair[0].glyphs.size () == 3);
    REQUIRE (pair[0].glyphs[1].face == noto);
    const auto pairPlaced = placeRow (pair[0].glyphs);
    const float gap = pairPlaced.x[2]
        - (pairPlaced.x[0] + static_cast<float> (pair[0].glyphs[0].glyph.xAdvance26_6 >> 6));
    REQUIRE (gap == static_cast<float> (shapeTextRun (noto, scalars (" ")).glyphs[0].xAdvance26_6 >> 6));
    REQUIRE (gap > 0.0f);

    const auto heart = layoutTextRows (scalars ("A 🧡 B"), resolve, none);
    REQUIRE (heart[0].glyphs.size () == 5);
    const std::vector<FT_Face> heartFaces {noto, noto, twemoji, noto, noto};
    for (size_t index = 0; index < heartFaces.size (); ++index) {
        REQUIRE (heart[0].glyphs[index].face == heartFaces[index]);
        REQUIRE ((heart[0].glyphs[index].glyph.xAdvance26_6 >> 6) > 0);
    }

    // The ellipsis trial measures the substituted space that is drawn, not
    // Twemoji's notdef advance.
    const auto emojiRow = scalars ("😀 AAAA");
    const float emojiFit = placeRow (shapeNativeTextRow (scalars ("😀 A…"), resolve).glyphs).right.back ();
    REQUIRE (rowTexts (layoutTextRows (emojiRow, resolve, widthLimit (emojiFit, 1, true)))
             == Rows {scalars ("😀 A…")});

    // Native run-boundary-emoji: an exhausted scalar after an emoji takes no advance.
    const auto exhausted = placeRow (layoutTextRows (scalars ("😀͸A"), resolve, none)[0].glyphs);
    const auto control = placeRow (layoutTextRows (scalars ("😀A"), resolve, none)[0].glyphs);
    REQUIRE (exhausted.x == control.x);

    // Native marks-across-runs: RobotoMono lacks U+0308, so A+U+0308 composes to Ä.
    REQUIRE (FT_Get_Char_Index (roboto, 0x0308) == 0);
    const auto robotoResolve = resolverFor ({roboto});
    const auto decomposed = layoutTextRows (scalars ("Ä"), robotoResolve, none);
    REQUIRE (decomposed[0].glyphs.size () == 1);
    REQUIRE (decomposed[0].glyphs[0].glyph.glyphIndex == FT_Get_Char_Index (roboto, 0x00c4));

    // Native fixture outcomes at RobotoMono 24 pt (raster pixels).
    REQUIRE (layoutTextRows (scalars ("aaaaaaa."), robotoResolve, widthLimit (469.0f)).size () == 1);
    REQUIRE (rowTexts (layoutTextRows (scalars ("aaaaaaaa"), robotoResolve, widthLimit (469.0f)))
             == Rows {scalars ("aaaaaaa"), scalars ("a")});
    REQUIRE (rowTexts (layoutTextRows (scalars ("  MMMMMMMMMMMM"), robotoResolve, widthLimit (450.0f)))
             == Rows {scalars (""), scalars ("MMMMMMM"), scalars ("MMMMM")});
    REQUIRE (rowTexts (layoutTextRows (scalars ("aaaa bbbb cccc"), robotoResolve, widthLimit (450.0f, 1)))
             == Rows {scalars ("aaaa bb")});
    REQUIRE (rowTexts (layoutTextRows (scalars ("aaaa bbbb cccc"), robotoResolve, widthLimit (450.0f, 1, true)))
             == Rows {scalars ("aaaa b…")});
    REQUIRE (rowTexts (layoutTextRows (scalars ("aaaa\nbbbbbbbbbb\ncccc"), robotoResolve,
                                       widthLimit (450.0f, 2, true)))
             == Rows {scalars ("aaaa"), scalars ("bbbbbb…")});
    REQUIRE (rowTexts (layoutTextRows (scalars ("aaaa bbbb\ncccc"), robotoResolve, widthLimit (450.0f, 2, true)))
             == Rows {scalars ("aaaa"), scalars ("bbbb…")});
    struct Case { const char* text; const char* first; const char* second; };
    for (const Case& value : {Case {"aaaa bbbb", "aaaa", "bbbb"}, Case {"aaaa-bbbb", "aaaa-bb", "bb"},
                              Case {"aaaa bbbb", "aaaa bb", "bb"},
                              Case {"aaaa​bbbb", "aaaa​bbb", "b"}}) {
        INFO (value.text);
        REQUIRE (rowTexts (layoutTextRows (scalars (value.text), robotoResolve, widthLimit (450.0f)))
                 == Rows {scalars (value.first), scalars (value.second)});
    }
}

namespace {
TextLayoutLimits spacedLimit (float maxWidth, float spacing, int maxRows = 0, bool ellipsis = false) {
    auto limits = widthLimit (maxWidth, maxRows, ellipsis);
    limits.letterSpacing = spacing;
    return limits;
}
} // namespace

TEST_CASE ("Native wrap includes letter spacing in the pen but the ellipsis trial excludes it", "[text][layout][wrap]") {
    TestFonts fonts;
    const FT_Face sans = fonts.loadSystem ("DejaVuSans.ttf");
    const auto resolve = resolverFor ({sans});
    constexpr float spacing = 8.0f;

    const auto eight = scalars ("aaaaaaaa");
    const auto glyphs = shapeNativeTextRow (eight, resolve).glyphs;
    const float advance = static_cast<float> (glyphs[0].glyph.xAdvance26_6 >> 6);
    // The eighth glyph's ink with seven spaced advances before it.
    const float spacedRight = placeRow (glyphs).right.back () + 7.0f * spacing;
    REQUIRE (layoutTextRows (eight, resolve, spacedLimit (spacedRight, spacing)).size () == 1);
    REQUIRE (rowTexts (layoutTextRows (eight, resolve, spacedLimit (spacedRight - 1.0f, spacing)))
             == Rows {scalars ("aaaaaaa"), scalars ("a")});
    REQUIRE (spacedRight - 7.0f * spacing < spacedRight - 1.0f);
    REQUIRE (advance > 0.0f);

    // The spaced walk overflows at the second b; the unspaced trial still fits "aaaa b…".
    const float unspacedFit = placeRow (shapeNativeTextRow (scalars ("aaaa b…"), resolve).glyphs).right.back ();
    REQUIRE (rowTexts (layoutTextRows (scalars ("aaaa bbbb cccc"), resolve, spacedLimit (unspacedFit, spacing, 1, true)))
             == Rows {scalars ("aaaa b…")});
}

TEST_CASE ("Native wrap never breaks inside one shaped cluster", "[text][layout][wrap]") {
    TestFonts fonts;
    const FT_Face sans = fonts.loadSystem ("DejaVuSans.ttf");
    const auto resolve = resolverFor ({sans});
    // U+035D is a double diacritic: its ink extends right of its base glyph,
    // but it shares the base's cluster, so it cannot trigger a break.
    const auto text = scalars ("aa͝");
    const auto glyphs = shapeNativeTextRow (text, resolve).glyphs;
    REQUIRE (glyphs.size () == 3);
    REQUIRE (glyphs[1].glyph.cluster == glyphs[2].glyph.cluster);
    const auto placed = placeRow (glyphs);
    REQUIRE (placed.right[2] > placed.right[1] + 1.0f);
    const auto rows = layoutTextRows (text, resolve, widthLimit (placed.right[1]));
    REQUIRE (rows.size () == 1);
    REQUIRE (rows[0].glyphs.size () == 3);
}

TEST_CASE ("A wrap pops every trailing space and reports unsafe splits", "[text][layout][wrap]") {
    TestFonts fonts;
    const FT_Face sans = fonts.loadSystem ("DejaVuSans.ttf");
    const auto resolve = resolverFor ({sans});
    const auto text = scalars ("aaaa  bbbb");
    const float maxWidth = inkRightOfAdvancingGlyph (shapeNativeTextRow (text, resolve).glyphs, 7);
    const auto rows = layoutTextRows (text, resolve, widthLimit (maxWidth));
    REQUIRE (rowTexts (rows) == Rows {scalars ("aaaa"), scalars ("bbbb")});
    // The tail pop uses the trimmed length: neither space stays in the bounds.
    REQUIRE (rows[0].glyphs.size () == 4);

    // reprocess only when glyphs remain in the popped
    // run and the last popped glyph is unsafe to break.
    using WallpaperEngine::Render::Objects::detail::dropTextGlyphTail;
    const auto run = [] (std::initializer_list<bool> unsafe) {
        std::vector<TextRowGlyph> glyphs;
        uint32_t cluster = 0;
        for (const bool flag : unsafe)
            glyphs.push_back ({nullptr, {1, cluster++, 64, 0, 0, 0, flag}, 0});
        return glyphs;
    };
    auto kept = run ({false, false, false, true, false});
    REQUIRE (dropTextGlyphTail (kept, 3));
    REQUIRE (kept.size () == 3);
    kept = run ({false, false, false, false, true});
    REQUIRE_FALSE (dropTextGlyphTail (kept, 3));
    kept = run ({true, false, false});
    REQUIRE_FALSE (dropTextGlyphTail (kept, 0));
    REQUIRE (kept.empty ());
}

TEST_CASE ("The native ellipsis trial measures notdef glyphs and keeps glyphs when nothing fits", "[text][layout][maxrows]") {
    TestFonts fonts;
    const FT_Face sans = fonts.loadSystem ("DejaVuSans.ttf");
    const auto resolve = resolverFor ({sans});

    // Drawn rows skip the notdef of U+0378, but the trial counts its advance.
    const float drawnFit = placeRow (shapeNativeTextRow (scalars ("aaaa͸b…"), resolve).glyphs).right.back ();
    auto rows = layoutTextRows (scalars ("aaaa͸bbbb"), resolve, widthLimit (drawnFit, 1, true));
    REQUIRE (rowTexts (rows) == Rows {scalars ("aaaa͸…")});
    REQUIRE (rows[0].glyphs.size () == 5);

    // When no prefix plus "…" fits, the kept glyphs stay and no ellipsis is added.
    const float oneGlyph = placeRow (shapeNativeTextRow (scalars ("M"), resolve).glyphs).right.back ();
    rows = layoutTextRows (scalars ("MMMM"), resolve, widthLimit (oneGlyph + 1.0f, 1, true));
    REQUIRE (rowTexts (rows) == Rows {scalars ("M")});
    REQUIRE (rows[0].glyphs.size () == 1);
    REQUIRE (rows[0].glyphs[0].glyph.glyphIndex == FT_Get_Char_Index (sans, 'M'));
}

TEST_CASE ("The row cap ellipsizes the last row kept by the Linux RTL wrap", "[text][layout][wrap][rtl]") {
    TestFonts fonts;
    const FT_Face sans = fonts.loadSystem ("DejaVuSans.ttf");
    const auto resolve = resolverFor ({sans});
    float widestWord = 0.0f;
    for (const char* word : {"مرحبا", "بالعالم"}) {
        float width = 0.0f;
        for (const auto& glyph : shapeTextRun (sans, scalars (word)).glyphs)
            width += static_cast<float> (glyph.xAdvance26_6 >> 6);
        widestWord = std::max (widestWord, width);
    }
    const auto rows = layoutTextRows (scalars ("مرحبا بالعالم مرحبا"), resolve,
                                      widthLimit (widestWord + 1.0f, 2, true));
    REQUIRE (rowTexts (rows) == Rows {scalars ("مرحبا"), scalars ("بالعالم…")});
}

TEST_CASE ("A fallback-supplied mark stays in its base run when the run face composes it", "[text][layout][runs][shipped]") {
    const char* root = std::getenv ("WPE_SHIPPED_ASSETS");
    if (!root) SKIP ("WPE_SHIPPED_ASSETS is not set; shipped RobotoMono/NotoSans mark composition not run");
    const std::filesystem::path assets (root);
    TestFonts fonts;
    const FT_Face roboto = fonts.load (assets / "fonts/RobotoMono-Regular.ttf");
    const FT_Face noto = fonts.load (assets / "fonts/NotoSans-Regular.ttf");
    REQUIRE (FT_Get_Char_Index (roboto, 0x0308) == 0);
    REQUIRE (FT_Get_Char_Index (noto, 0x0308) != 0);
    // As on Linux, a fallback face supplies the mark that RobotoMono lacks.
    const auto resolve = resolverFor ({roboto, noto});
    const TextLayoutLimits none;

    // Native marks-across-runs: the mark is exhausted there, stays in the base
    // run and composes; Linux keeps it in the run because RobotoMono has Ä.
    const auto composed = layoutTextRows (scalars ("A\xcc\x88"), resolve, none);
    REQUIRE (composed[0].glyphs.size () == 1);
    REQUIRE (composed[0].glyphs[0].face == roboto);
    REQUIRE (composed[0].glyphs[0].glyph.glyphIndex == FT_Get_Char_Index (roboto, 0x00c4));

    // The following space then keeps the base face's advance.
    const auto cafe = layoutTextRows (scalars ("Cafe\xcc\x81 A\xcc\x88 n\xcc\x83"), resolve, none);
    for (const auto& glyph : cafe[0].glyphs) {
        REQUIRE (glyph.face == roboto);
        REQUIRE (glyph.runStart == 0);
    }
    const auto precomposed = layoutTextRows (scalars ("Café Ä ñ"), resolve, none);
    REQUIRE (placeRow (cafe[0].glyphs).x == placeRow (precomposed[0].glyphs).x);

    // Without a precomposed form in the run face the Fontconfig-style
    // fallback stays: Q+U+0308 draws the NotoSans mark in its own run.
    REQUIRE (FT_Get_Char_Index (roboto, 0x0051) != 0);
    hb_codepoint_t q = 0;
    REQUIRE_FALSE (hb_unicode_compose (hb_unicode_funcs_get_default (), 'Q', 0x0308, &q));
    const auto fallback = layoutTextRows (scalars ("Q\xcc\x88"), resolve, none);
    REQUIRE (fallback[0].glyphs.size () == 2);
    REQUIRE (fallback[0].glyphs[0].face == roboto);
    REQUIRE (fallback[0].glyphs[1].face == noto);
    REQUIRE (fallback[0].glyphs[1].runStart == 1);

    // A canonical composition the run face does not cover (A+U+030C = U+01CD)
    // also keeps the fallback mark.
    REQUIRE (FT_Get_Char_Index (roboto, 0x01cd) == 0);
    REQUIRE (FT_Get_Char_Index (roboto, 0x030c) == 0);
    REQUIRE (FT_Get_Char_Index (noto, 0x030c) != 0);
    hb_codepoint_t caron = 0;
    REQUIRE (hb_unicode_compose (hb_unicode_funcs_get_default (), 'A', 0x030c, &caron));
    REQUIRE (caron == 0x01cd);
    const auto uncovered = layoutTextRows (scalars ("A\xcc\x8c"), resolve, none);
    REQUIRE (uncovered[0].glyphs.size () == 2);
    REQUIRE (uncovered[0].glyphs[1].face == noto);
}

namespace {
TextLayoutBox layoutBoxOf (FT_Face face, std::string_view text, float letterSpacing = 0.0f,
                           float rowSpacing = 0.0f) {
    const TextLayoutLimits none;
    return textLayoutBox (layoutTextRows (scalars (text), resolverFor ({face}), none), letterSpacing,
                          static_cast<float> (face->size->metrics.height >> 6) + rowSpacing,
                          static_cast<float> (face->size->metrics.ascender >> 6));
}
} // namespace

TEST_CASE ("The text layout box reproduces the boxes measured in native frames", "[text][layout][box][shipped]") {
    const char* root = std::getenv ("WPE_SHIPPED_ASSETS");
    if (!root) SKIP ("WPE_SHIPPED_ASSETS is not set; shipped RobotoMono box fixtures not run");
    TestFonts fonts;
    const FT_Face roboto = fonts.load (std::filesystem::path (root) / "fonts/RobotoMono-Regular.ttf");
    // W17 run2 native captures (w17-persp-offc, opaque background, padding
    // 24, centre/centre): Hxg 220x180 and HHHH 281x180 at 24 pt, Hxg 134x114
    // at 12 pt. The Linux raster boxes were 228x140, 288x119 and 138x94.
    const float pad = textLayerPadding (24.0f, false, true);
    const auto hxg = layoutBoxOf (roboto, "Hxg");
    REQUIRE (hxg.width () + 2.0f * pad == 220.0f);
    REQUIRE (hxg.height () + 2.0f * pad == 180.0f);
    const auto hhhh = layoutBoxOf (roboto, "HHHH");
    REQUIRE (hhhh.width () + 2.0f * pad == 281.0f);
    REQUIRE (hhhh.height () + 2.0f * pad == 180.0f);
    // The trailing advance is not part of the box.
    REQUIRE (hxg.width () < placeRow (layoutTextRows (scalars ("Hxg"), resolverFor ({roboto}), {})[0].glyphs).pen);
    // Centre alignment centres the box, not the ink: its centre sat 13 px
    // below the layer origin (418 versus 405 in the frame).
    const float ascender = static_cast<float> (roboto->size->metrics.ascender >> 6);
    const float descender = static_cast<float> (roboto->size->metrics.descender >> 6);
    const float lineHeight = static_cast<float> (roboto->size->metrics.height >> 6);
    REQUIRE (textVerticalAnchorOffset (hxg.centerY (), ascender, descender, 1, lineHeight, "center") == -13.5f);
    REQUIRE (FT_Set_Char_Size (roboto, 0, 12 * 64, 300, 300) == 0);
    const auto small = layoutBoxOf (roboto, "Hxg");
    REQUIRE (small.width () + 2.0f * pad == 134.0f);
    REQUIRE (small.height () + 2.0f * pad == 114.0f);
    // w31-run1 native frame (padding 0): Alcubierre 12 pt grave/acute
    // capitals rise above the ascender and the box rises with them, 67 px tall
    // where the line box is 59.
    const FT_Face display = fonts.load (std::filesystem::path (root) / "fonts/Alcubierre.otf");
    REQUIRE (FT_Set_Char_Size (display, 0, 12 * 64, 300, 300) == 0);
    REQUIRE ((display->size->metrics.height >> 6) == 59);
    REQUIRE (layoutBoxOf (display, "\xc3\x80\xc3\x81").height () == 67.0f);
}

TEST_CASE ("Text layout box rows take whole line heights while ink extends them", "[text][layout][box]") {
    TestFonts fonts;
    const FT_Face sans = fonts.loadSystem ("DejaVuSans.ttf");
    const float ascender = static_cast<float> (sans->size->metrics.ascender >> 6);
    const float lineHeight = static_cast<float> (sans->size->metrics.height >> 6);

    // Every row, the last included, takes the full line height plus the
    // authored row spacing; the top is the ascender.
    const auto rows = layoutBoxOf (sans, "A\nA\nA", 0.0f, 7.5f);
    REQUIRE (rows.maxY == ascender);
    REQUIRE (rows.height () == 3.0f * (lineHeight + 7.5f));

    // Overlapping rows: the box ends at the last row's baseline, not at a
    // line box that would end above it.
    const float overlap = lineHeight - 100.0f;
    REQUIRE (ascender - 3.0f * overlap > -2.0f * overlap);
    REQUIRE (layoutBoxOf (sans, "A\nA\nA", 0.0f, -100.0f).minY == -2.0f * overlap);

    // A descender below the line box extends it.
    FT_BBox g {};
    REQUIRE (textGlyphPixelBounds (sans, FT_Get_Char_Index (sans, 'g'), g));
    REQUIRE (static_cast<float> (g.yMin) < ascender - overlap);
    REQUIRE (layoutBoxOf (sans, "g", 0.0f, -100.0f).minY == static_cast<float> (g.yMin));

    // Ink above the ascender raises the top to the ink; the bottom keeps
    // the line box.
    FT_BBox tall {};
    REQUIRE (textGlyphPixelBounds (sans, FT_Get_Char_Index (sans, 0x01d9), tall));
    REQUIRE (static_cast<float> (tall.yMax) > ascender);
    const auto stacked = layoutBoxOf (sans, "\xc7\x99");
    REQUIRE (stacked.maxY == static_cast<float> (tall.yMax));
    REQUIRE (stacked.minY == ascender - lineHeight);
}

TEST_CASE ("Text layout box width counts trailing spaces to their pen and letter spacing", "[text][layout][box]") {
    TestFonts fonts;
    const FT_Face sans = fonts.loadSystem ("DejaVuSans.ttf");
    const TextLayoutLimits none;
    const auto spaced = layoutTextRows (scalars ("A  "), resolverFor ({sans}), none);
    const auto placed = placeRow (spaced[0].glyphs);
    const float space = static_cast<float> (spaced[0].glyphs.back ().glyph.xAdvance26_6 >> 6);
    REQUIRE (space > 0.0f);
    // The last space adds its pen position but not its advance.
    REQUIRE (layoutBoxOf (sans, "A  ").width () == placed.pen - space);
    REQUIRE (layoutBoxOf (sans, "A  ").width () > layoutBoxOf (sans, "A").width ());
    REQUIRE (layoutBoxOf (sans, "AA", 10.0f).width () == layoutBoxOf (sans, "AA").width () + 10.0f);
}

TEST_CASE ("Text padding is a margin only for effects or an opaque background", "[text][layout][box]") {
    REQUIRE (textLayerPadding (24.0f, false, false) == 0.0f);
    REQUIRE (textLayerPadding (24.0f, true, false) == 24.0f);
    REQUIRE (textLayerPadding (24.0f, false, true) == 24.0f);
    REQUIRE (textLayerPadding (600.0f, true, true) == 512.0f);
    REQUIRE (textLayerPadding (-5.0f, true, false) == -5.0f);
}
