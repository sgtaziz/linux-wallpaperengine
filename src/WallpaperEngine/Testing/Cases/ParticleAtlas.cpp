#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Render/Objects/ParticleAtlas.h"
#include "WallpaperEngine/Render/Shaders/GLSLContext.h"
#include "WallpaperEngine/Render/Shaders/ShaderUnit.h"
#include "WallpaperEngine/Render/Shaders/ParticleSpriteShader.h"
#include "WallpaperEngine/Render/TextureAnimation.h"

#include <catch2/catch_approx.hpp>
#include <glm/vec2.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

using namespace WallpaperEngine::Render::Objects::ParticleAtlas;

namespace {
// TEXS rects of a row-major sheet (every atlas below was decoded from its
// shipped/installed .tex and its TEXS records follow this layout exactly).
Source sheet (uint32_t mipWidth, uint32_t mipHeight, uint32_t imageWidth, uint32_t imageHeight,
              float frameWidth, float frameHeight, uint32_t columns, uint32_t count) {
    Source source {mipWidth, mipHeight, imageWidth, imageHeight, {}};
    for (uint32_t frame = 0; frame < count; ++frame)
        source.authored.emplace_back (static_cast<float> (frame % columns) * frameWidth,
                                      static_cast<float> (frame / columns) * frameHeight, frameWidth, frameHeight);
    return source;
}

// Coverage page built from the decoded mip0 alpha at cell granularity:
// '#' cells contain nonzero coverage, '.' cells are fully transparent.
struct CellCoverage {
    std::vector<uint8_t> alpha;
    uint32_t width, height;

    CellCoverage (uint32_t pageWidth, uint32_t pageHeight, uint32_t cellWidth, uint32_t cellHeight,
                  const std::vector<std::string>& cells) :
        alpha (static_cast<size_t> (pageWidth) * pageHeight, 0), width (pageWidth), height (pageHeight) {
        for (uint32_t y = 0; y < pageHeight; ++y)
            for (uint32_t x = 0; x < pageWidth; ++x) {
                const auto row = y / cellHeight, column = x / cellWidth;
                if (row < cells.size () && column < cells[row].size () && cells[row][column] == '#')
                    alpha[static_cast<size_t> (y) * pageWidth + x] = 255;
            }
    }
    CoverageQuery query () const {
        return [this] (const TexelRect& rect) { return rectHasCoverage (alpha.data (), width, height, 1, 0, rect); };
    }
};

TexelRect texels (const Source& source, const glm::vec4& window) {
    return window * glm::vec4 (static_cast<float> (source.mipWidth), static_cast<float> (source.mipHeight),
                               static_cast<float> (source.mipWidth), static_cast<float> (source.mipHeight));
}

void requireRect (const TexelRect& actual, float x, float y, float w, float h) {
    REQUIRE (actual.x == Catch::Approx (x).margin (1e-3));
    REQUIRE (actual.y == Catch::Approx (y).margin (1e-3));
    REQUIRE (actual.z == Catch::Approx (w).margin (1e-3));
    REQUIRE (actual.w == Catch::Approx (h).margin (1e-3));
}

const CoverageQuery allCovered = [] (const TexelRect&) { return true; };
}

TEST_CASE ("Padded particle atlas samples authored-size native windows", "[particle][atlas]") {
    // workshop/2941380183/particle/notes_130x285_4 (Lofi 2370927443): mip0
    // 1024x512 padded data, image 520x258, four 130x285 frames in one row.
    // Native-backed: captures show 130x285 windows; the old 4x1 grid sampled 256x512.
    const auto source = sheet (1024, 512, 520, 258, 130.0f, 285.0f, 4, 4);
    const auto atlas = layout (source, allCovered);
    REQUIRE (atlas.windows.size () == 4);
    for (uint32_t frame = 0; frame < 4; ++frame) {
        REQUIRE (atlas.sources[frame] == WindowSource::Native);
        requireRect (texels (source, atlas.windows[frame]), 130.0f * frame, 0.0f, 130.0f, 285.0f);
    }
    // g_RenderVar1.w = mipH/mipW * fh/fw = the frame's pixel aspect 285/130.
    REQUIRE (atlas.renderVar1.x == 130.0f / 1024.0f);
    REQUIRE (atlas.renderVar1.y == 285.0f / 512.0f);
    REQUIRE (atlas.renderVar1.z == 4.0f);
    REQUIRE (atlas.renderVar1.w == Catch::Approx (2.1923077f).epsilon (1e-6));
}

TEST_CASE ("Padded multi-row atlas keeps authored tiles except over empty padding", "[particle][atlas]") {
    // workshop/2214863259/particle/matrix spritesheet 72 (2938612768,
    // 3238423642): mip0 512x512, image 450x400, 71 TEXS frames of 50 px in
    // rows of ten whose tenth column (x = 450) lies in transparent padding.
    // Native NPOT windows (image < mip0) use nine columns. Native-backed: frames 0-8.
    const auto source = sheet (512, 512, 450, 400, 50.0f, 50.0f, 10, 71);
    const CellCoverage coverage (512, 512, 50, 50, {
        "#########..", "#########..", "#########..", "#########..",
        "#########..", "#########..", "#########..", "#########..",
        "...........", "...........", "..........."});
    const auto atlas = layout (source, coverage.query ());
    REQUIRE (atlas.windows.size () == 71);
    for (uint32_t frame = 0; frame < 9; ++frame) {
        REQUIRE (atlas.sources[frame] == WindowSource::Native);
        requireRect (texels (source, atlas.windows[frame]), 50.0f * frame, 0.0f, 50.0f, 50.0f);
    }
    // Root ruling: every tenth authored rect is empty while the native window
    // shows a glyph cell, so native's window is used there.
    const std::vector<std::pair<uint32_t, glm::vec2>> emptyAuthored = {
        {9, {0, 50}}, {19, {50, 100}}, {29, {100, 150}}, {39, {150, 200}},
        {49, {200, 250}}, {59, {250, 300}}, {69, {300, 350}}};
    for (const auto& [frame, origin] : emptyAuthored) {
        REQUIRE (atlas.sources[frame] == WindowSource::NativeOverEmptyAuthored);
        requireRect (texels (source, atlas.windows[frame]), origin.x, origin.y, 50.0f, 50.0f);
    }
    // Deliberate divergence (rule 2): native mis-indexes the other frames >= 10;
    // Linux keeps the authored tiles.
    for (uint32_t frame = 10; frame < 71; ++frame) {
        if (frame % 10 == 9) continue;
        REQUIRE (atlas.sources[frame] == WindowSource::Authored);
        requireRect (texels (source, atlas.windows[frame]), 50.0f * (frame % 10), 50.0f * (frame / 10), 50.0f, 50.0f);
    }
    requireRect (texels (source, nativeWindow (source, 18)), 0.0f, 100.0f, 50.0f, 50.0f);
    REQUIRE (atlas.renderVar1.w == 1.0f);
}

TEST_CASE ("Bird atlases follow native windows only where they match TEXS", "[particle][atlas]") {
    SECTION ("non-dividing bird_2 (2384641783)") {
        // 1024x512 unpadded, eight 206 px frames in rows of four. Native places
        // 4.97 frames per row: frame 4 samples empty x = 824 on row 0.
        // Native-backed: frames 0-3. Deliberate divergence (rule 2): frames 4-7 keep TEXS.
        const auto source = sheet (1024, 512, 1024, 512, 206.0f, 206.0f, 4, 8);
        const CellCoverage coverage (1024, 512, 206, 206, {"####.", "####.", "....."});
        const auto atlas = layout (source, coverage.query ());
        requireRect (texels (source, nativeWindow (source, 4)), 824.0f, 0.0f, 206.0f, 206.0f);
        for (uint32_t frame = 0; frame < 8; ++frame) {
            REQUIRE (atlas.sources[frame] == (frame < 4 ? WindowSource::Native : WindowSource::Authored));
            requireRect (texels (source, atlas.windows[frame]), 206.0f * (frame % 4), 206.0f * (frame / 4),
                         206.0f, 206.0f);
        }
        REQUIRE (atlas.renderVar1.w == 1.0f);
    }
    SECTION ("bird sprite sheet under an NPOT header (2524733204 and others)") {
        // TEXV 2048x1024, image and uploaded mip0 1603x616: the texture object
        // takes mip0, so NPOT is off. 24 frames of 200x205, eight per row.
        // Native-backed: frames 0-7 and ratio 1.025. Deliberate divergence
        // (rule 2): frames 8-23 keep TEXS (native smears 8/16 and drifts 3-6 texels).
        const auto source = sheet (1603, 616, 1603, 616, 200.0f, 205.0f, 8, 24);
        const CellCoverage coverage (1603, 616, 200, 205, {"#########", "#########", "#########", "....#...."});
        const auto atlas = layout (source, coverage.query ());
        requireRect (texels (source, nativeWindow (source, 8)), 1600.0f, 0.0f, 200.0f, 205.0f);
        requireRect (texels (source, nativeWindow (source, 9)), 197.0f, 205.0f, 200.0f, 205.0f);
        for (uint32_t frame = 0; frame < 24; ++frame) {
            REQUIRE (atlas.sources[frame] == (frame < 8 ? WindowSource::Native : WindowSource::Authored));
            requireRect (texels (source, atlas.windows[frame]), 200.0f * (frame % 8), 205.0f * (frame / 8),
                         200.0f, 205.0f);
        }
        REQUIRE (atlas.renderVar1.w == Catch::Approx (1.025f).epsilon (1e-6));
    }
    SECTION ("accepted birdspritesheet1 cadence (3217188597 family)") {
        // TEXV 1024x1024 with an unpadded 516x516 mip0: native windows equal
        // the authored 129 px tiles for every frame.
        const auto source = sheet (516, 516, 516, 516, 129.0f, 129.0f, 4, 16);
        const auto atlas = layout (source, allCovered);
        for (uint32_t frame = 0; frame < 16; ++frame) {
            REQUIRE (atlas.sources[frame] == WindowSource::Native);
            requireRect (texels (source, atlas.windows[frame]), 129.0f * (frame % 4), 129.0f * (frame / 4),
                         129.0f, 129.0f);
        }
    }
    SECTION ("padded BIRD SPRITE (3055009324) through the NPOT branch") {
        // DXT5 mip0 512x880, image 500x868, eight 250x217 frames, two per row.
        const auto source = sheet (512, 880, 500, 868, 250.0f, 217.0f, 2, 8);
        const auto atlas = layout (source, allCovered);
        for (uint32_t frame = 0; frame < 8; ++frame) {
            REQUIRE (atlas.sources[frame] == WindowSource::Native);
            requireRect (texels (source, atlas.windows[frame]), 250.0f * (frame % 2), 217.0f * (frame / 2),
                         250.0f, 217.0f);
        }
        REQUIRE (atlas.renderVar1.w == Catch::Approx (0.868f).epsilon (1e-6));
    }
}

TEST_CASE ("Off-page TEXS layouts keep the native page tiling", "[particle][atlas]") {
    SECTION ("accepted leaves (particle/nature/leaves7)") {
        // 512x512, 30 frames of 85.334x102.4. The shipped TEXS rows hold five
        // frames and put frames 25-29 at y = 512, off the page; the image and
        // native windows tile six columns (the user-accepted leaves cadence).
        const auto source = sheet (512, 512, 512, 512, 85.334f, 102.4f, 5, 30);
        const auto atlas = layout (source, allCovered);
        REQUIRE (atlas.windows.size () == 30);
        for (uint32_t frame = 0; frame < 30; ++frame) {
            REQUIRE (atlas.sources[frame] == (frame < 5 ? WindowSource::Native
                                                        : WindowSource::NativeOverOffPageLayout));
            const auto window = texels (source, atlas.windows[frame]);
            REQUIRE (window.y == Catch::Approx (102.4f * (frame / 6)).margin (1e-3));
            REQUIRE (window.x == Catch::Approx (85.334f * (frame % 6)).margin (0.05));
        }
        REQUIRE (atlas.renderVar1.w == Catch::Approx (1.2f).epsilon (1e-4));
    }
    SECTION ("rosepetals (particle/nature/rosepetals)") {
        // 512x128, five 102.5x128 frames; TEXS frame 4 sits at y = 128.
        const auto source = sheet (512, 128, 512, 128, 102.5f, 128.0f, 4, 5);
        const auto atlas = layout (source, allCovered);
        REQUIRE (atlas.sources[4] == WindowSource::NativeOverOffPageLayout);
        requireRect (texels (source, atlas.windows[4]), 410.0f, 0.0f, 102.5f, 128.0f);
    }
}

TEST_CASE ("Narrow grass atlas windows stay on authored texels", "[particle][atlas]") {
    // workshop/2914071577/particle/grass_x4 (2915841260): 147x300, three
    // 36x300 frames. The old round(147/36) = 4 column grid placed frame 2 at
    // 73.5 texels with 36.75-texel windows; native and TEXS place it at 72.
    const auto source = sheet (147, 300, 147, 300, 36.0f, 300.0f, 3, 3);
    const auto atlas = layout (source, allCovered);
    for (uint32_t frame = 0; frame < 3; ++frame) {
        REQUIRE (atlas.sources[frame] == WindowSource::Native);
        requireRect (texels (source, atlas.windows[frame]), 36.0f * frame, 0.0f, 36.0f, 300.0f);
    }
    REQUIRE (atlas.renderVar1.w == Catch::Approx (300.0f / 36.0f).epsilon (1e-6));
}

TEST_CASE ("Same-page atlas sequences ignore TEXS frame durations", "[particle][atlas][animation]") {
    // W12 atlas-unequal-durations: confetti TEXS durations [.4, .0625 x 15],
    // lifetime 32, multiplier 1. Native frames advance uniformly every 2 s
    // (in_ParticleLifeTime = age * rcp(life) * multiplier).
    using WallpaperEngine::Render::particleSequenceCycles;
    for (uint32_t frame = 0; frame < 16; ++frame) {
        const double age = 2.0 * frame + 1.0;
        const float position = sequenceFrame (particleSequenceCycles (age, 32.0, 1.0), 16, false);
        REQUIRE (sampleFrames (position / 16.0f, 16).current == frame);
        REQUIRE (static_cast<uint32_t> (position) == frame);
    }
    // An authored timeline would still hold frame 0 at 9.6 s.
    REQUIRE (sequenceFrame (particleSequenceCycles (9.6, 32.0, 1.0), 16, false) == Catch::Approx (4.8f));
    // Once mode clamps on the last frame; sequences wrap.
    REQUIRE (sequenceFrame (1.5, 16, true) == 15.0f);
    REQUIRE (sequenceFrame (1.25, 16, false) == Catch::Approx (4.0f));
}

TEST_CASE ("Atlas frame sampling follows ComputeSpriteFrame", "[particle][atlas]") {
    const auto middle = sampleFrames (0.53125f, 16);
    REQUIRE (middle.current == 8);
    REQUIRE (middle.next == 9);
    REQUIRE (middle.blend == Catch::Approx (0.5f));
    const auto last = sampleFrames (0.99f, 16);
    REQUIRE (last.current == 15);
    REQUIRE (last.next == 15);
    // genericparticle.vert passes frac(in_ParticleLifeTime).
    REQUIRE (sampleFrames (2.25f, 4).current == 1);
    REQUIRE (sampleFrames (std::numeric_limits<float>::quiet_NaN (), 4).current == 0);
}

TEST_CASE ("Atlas coverage inspects the texels a rectangle covers", "[particle][atlas]") {
    std::vector<uint8_t> rgba (4 * 4 * 4, 0);
    rgba[(2 * 4 + 3) * 4 + 3] = 1; // alpha at texel (3, 2)
    REQUIRE (rectHasCoverage (rgba.data (), 4, 4, 4, 3, {3.0f, 2.0f, 1.0f, 1.0f}));
    REQUIRE (rectHasCoverage (rgba.data (), 4, 4, 4, 3, {2.5f, 1.5f, 1.0f, 1.0f}));
    REQUIRE_FALSE (rectHasCoverage (rgba.data (), 4, 4, 4, 3, {0.0f, 0.0f, 3.0f, 4.0f}));
    REQUIRE_FALSE (rectHasCoverage (rgba.data (), 4, 4, 4, 0, {3.0f, 2.0f, 1.0f, 1.0f}));
    // Rectangles outside the page sample nothing.
    REQUIRE_FALSE (rectHasCoverage (rgba.data (), 4, 4, 4, 3, {4.0f, 0.0f, 2.0f, 4.0f}));
}

TEST_CASE ("Sprite atlas CPU UV override replaces the shipped frame grid", "[particle][atlas][shader]") {
    // Lines of the shipped assets/shaders/genericparticle.vert (2.8.42).
    const std::string native =
        "#if SPRITESHEET\n"
        "\tvec2 uvOffsets;\n"
        "\tComputeSpriteFrame(frac(in_ParticleLifeTime), v_TexCoord, uvOffsets, v_TexCoordBlend);\n"
        "\tuvOffsets *= step(0.001, a_TexCoordVec4.xy);\n"
        "\tv_TexCoord += uvOffsets.xyxy;\n"
        "#endif\n";
    const auto cpu = WallpaperEngine::Render::Shaders::particleSpriteCpuUVSource (native);
    REQUIRE (cpu);
    REQUIRE (cpu->find ("attribute vec4 a_CpuSpriteUV;") != std::string::npos);
    REQUIRE (cpu->find ("attribute float a_CpuSpriteBlend;") != std::string::npos);
    REQUIRE (cpu->find ("v_TexCoord = a_CpuSpriteUV; v_TexCoordBlend = a_CpuSpriteBlend;") != std::string::npos);
    REQUIRE (cpu->find ("ComputeSpriteFrame(") == std::string::npos);
    REQUIRE (cpu->find ("uvOffsets.xyxy") == std::string::npos);
    REQUIRE_FALSE (WallpaperEngine::Render::Shaders::particleSpriteCpuUVSource ("void main() {}"));
}

TEST_CASE ("Shipped genericparticle compiles with CPU atlas windows", "[particle][atlas][shader][shipped]") {
    using WallpaperEngine::Render::Shaders::GLSLContext;
    using WallpaperEngine::Render::Shaders::ShaderUnit;
    const char* root = std::getenv ("WPE_SHIPPED_ASSETS");
    if (!root) SKIP ("WPE_SHIPPED_ASSETS is not set; shipped genericparticle shaders not compiled");
    auto files = std::make_unique<WallpaperEngine::FileSystem::Container> ();
    files->mount (std::filesystem::path (root), "/");
    WallpaperEngine::Assets::AssetLocator assets (std::move (files));
    const std::string vertex = assets.vertexShader ("genericparticle");
    const std::string fragment = assets.fragmentShader ("genericparticle");
    const WallpaperEngine::Data::Model::ShaderConstantMap constants;
    const WallpaperEngine::Data::Model::TextureMap textures;
    const WallpaperEngine::Data::Model::ComboMap pass;
    for (const int blend : {0, 1}) {
        for (const int trail : {0, 1}) {
            for (const int cpu : {0, 1}) {
                WallpaperEngine::Data::Model::ComboMap overrides {
                    {"THICKFORMAT", 1}, {"SPRITESHEET", 1}, {"SPRITESHEETBLEND", blend}};
                if (trail) overrides["TRAILRENDERER"] = 1;
                if (cpu) overrides["LINUX_CPU_SPRITE_UV"] = 1;
                ShaderUnit vertexUnit (GLSLContext::UnitType_Vertex, "genericparticle", vertex,
                                       assets, constants, textures, textures, pass, overrides);
                ShaderUnit fragmentUnit (GLSLContext::UnitType_Fragment, "genericparticle", fragment,
                                         assets, constants, textures, textures, pass, overrides);
                const auto translated = GLSLContext::get ().toGlsl (vertexUnit.compile (), fragmentUnit.compile ());
                INFO ("blend " << blend << " trail " << trail << " cpu " << cpu);
                REQUIRE_FALSE (translated.first.empty ());
                REQUIRE_FALSE (translated.second.empty ());
                REQUIRE ((translated.first.find ("a_CpuSpriteUV") != std::string::npos) == (cpu == 1));
                if (blend) REQUIRE ((translated.first.find ("a_CpuSpriteBlend") != std::string::npos) == (cpu == 1));
            }
        }
    }
}

TEST_CASE ("Atlas layouts are built once per live texture page", "[particle][atlas]") {
    // Event children (CParticle::setup on spawn) must not re-read the page.
    struct Page {};
    LayoutCache<Page> cache;
    int builds = 0;
    const auto build = [&] {
        ++builds;
        return layout (sheet (512, 512, 512, 512, 128.0f, 128.0f, 4, 16), allCovered);
    };
    auto first = std::make_shared<const Page> ();
    const auto a = cache.get (first, 0, build);
    const auto b = cache.get (first, 0, build);
    REQUIRE (builds == 1);
    REQUIRE (a == b);
    REQUIRE (a->windows.size () == 16);
    cache.get (first, 1, build);
    REQUIRE (builds == 2);
    // A released texture's entries are dropped and never matched again.
    first.reset ();
    auto second = std::make_shared<const Page> ();
    cache.get (second, 0, build);
    REQUIRE (builds == 3);
    REQUIRE (cache.size () == 1);
}
