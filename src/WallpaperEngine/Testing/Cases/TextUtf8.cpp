#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Render/Objects/TextCodepoints.h"
#include "WallpaperEngine/Render/Objects/TextRaster.h"
#include "WallpaperEngine/Render/Objects/TextShaping.h"

#include <filesystem>
#include <vector>

using WallpaperEngine::Render::Objects::decodeTextCodepoints;
using WallpaperEngine::Render::Objects::textBitmapCoverage;
using WallpaperEngine::Render::Objects::compositeTextRgba;
using WallpaperEngine::Render::Objects::compositeTextOffscreenRgba;
using WallpaperEngine::Render::Objects::shapeTextRun;
using WallpaperEngine::Render::Objects::shapeTextRow;

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
