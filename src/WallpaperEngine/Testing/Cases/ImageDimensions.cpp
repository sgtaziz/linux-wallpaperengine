#include <catch2/catch_test_macros.hpp>
#include <algorithm>

#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Render/Objects/ImageDimensions.h"

using namespace WallpaperEngine::Data::Model;
using WallpaperEngine::Data::JSON::JSON;
using WallpaperEngine::Data::Parsers::ObjectParser;

TEST_CASE ("Native image logical defaults preserve explicit zero and authored omission",
           "[scene][image][size][parser]") {
    auto files = std::make_unique<WallpaperEngine::FileSystem::Container> ();
    files->getVFS ().add ("material.json", R"({"passes":[{"shader":"flat"}]})");
    files->getVFS ().add ("solid.json", R"({"material":"material.json","solidlayer":true})");
    files->getVFS ().add ("model.json", R"({"material":"material.json","width":80,"height":70})");
    files->getVFS ().add ("width.json", R"({"material":"material.json","width":80})");
    files->getVFS ().add ("project.json", R"({"material":"material.json","autosize":true,"passthrough":true,"projectlayer":true})");
    Project project {};
    project.assetLocator = std::make_unique<WallpaperEngine::Assets::AssetLocator> (std::move (files));
    // Native captured omitted solid is 1x1. Model dimensions are constructor
    // defaults, while explicit size overrides them (1401fac50/1401a3fc0).
    for (const auto& [model, expected] : {
             std::pair {"solid.json", glm::vec2 (1, 1)},
             std::pair {"model.json", glm::vec2 (80, 70)},
             std::pair {"width.json", glm::vec2 (80, 1)},
             std::pair {"project.json", glm::vec2 (1, 1)}}) {
        JSON authored {{"id", 1}, {"image", model}};
        auto object = ObjectParser::parse (authored, project);
        REQUIRE (object->as<Image> ()->size->value->getVec2 () == expected);
        REQUIRE (object->as<Image> ()->model->projectlayer == (model == std::string ("project.json")));
        auto config = JSON::parse (object->initialConfiguration);
        REQUIRE_FALSE (config.contains ("size"));
        config["id"] = 2;
        auto clone = ObjectParser::parse (config, project);
        REQUIRE (clone->as<Image> ()->size->value->getVec2 () == expected);
        REQUIRE (clone->as<Image> ()->model->projectlayer == object->as<Image> ()->model->projectlayer);

        for (const auto& [encoded, size] : {
                 std::pair {"0 0", glm::vec2 (0, 0)},
                 std::pair {"0 70", glm::vec2 (0, 70)},
                 std::pair {"80 0", glm::vec2 (80, 0)},
                 std::pair {"80 70", glm::vec2 (80, 70)}}) {
            authored["size"] = encoded;
            object = ObjectParser::parse (authored, project);
            REQUIRE (object->as<Image> ()->size->value->getVec2 () == size);
            config = JSON::parse (object->initialConfiguration);
            REQUIRE (config["size"] == encoded);
            config["id"] = 3;
            clone = ObjectParser::parse (config, project);
            REQUIRE (clone->as<Image> ()->size->value->getVec2 () == size);
        }
    }
}

TEST_CASE ("Native source-load dimensions separate manual geometry from autosize and backing",
           "[scene][image][size]") {
    using WallpaperEngine::Render::Objects::imageBackingDimensions;
    using WallpaperEngine::Render::Objects::ImageDimensionFlags;
    using WallpaperEngine::Render::Objects::loadedImageDimensions;
    const glm::vec2 texture (150, 150);
    const glm::vec2 projection (3840, 2160);
    // Actual native solid/manual zero controls remain blank, with logical
    // zeros intact. This must not become a scene-sized quad or a zero FBO.
    for (const auto size : {glm::vec2 (0, 0), glm::vec2 (0, 70), glm::vec2 (80, 0),
                           glm::vec2 (1, 1), glm::vec2 (80, 70)}) {
        const auto solid = loadedImageDimensions (size, {.solidlayer = true}, std::nullopt, projection);
        const auto manual = loadedImageDimensions (size, {}, texture, projection);
        REQUIRE (solid.logical == size);
        REQUIRE (solid.geometry == size);
        REQUIRE (manual.logical == size);
        REQUIRE (manual.geometry == size);
        const auto backing = solid.backing;
        REQUIRE (backing.x == std::max (4.0f, size.x));
        REQUIRE (backing.y == std::max (4.0f, size.y));
        REQUIRE (manual.backing == texture);
    }
    // Captured autosize controls resolve both omitted/default1 and explicit
    // zero/positive80x70 to the actual 150x150 source dimensions.
    for (const auto size : {glm::vec2 (1, 1), glm::vec2 (0, 0), glm::vec2 (80, 70)}) {
        const auto autoSize = loadedImageDimensions (size, {.autosize = true}, texture, projection);
        REQUIRE (autoSize.logical == texture);
        REQUIRE (autoSize.geometry == texture);
        REQUIRE (autoSize.backing == texture);
    }
    // Fullscreen sizing belongs to source-texture loading. The captured
    // source-less fullscreen solid with size0x0 remains degenerate.
    const auto noSource = loadedImageDimensions ({0, 0}, {.fullscreen = true, .solidlayer = true},
                                               std::nullopt, projection);
    REQUIRE (noSource.logical == glm::vec2 (0));
    REQUIRE (noSource.geometry == glm::vec2 (0));
    REQUIRE (noSource.backing == glm::vec2 (4));
    const auto fullscreen = loadedImageDimensions ({80, 70}, {.fullscreen = true}, texture, projection);
    REQUIRE (fullscreen.logical == texture);
    REQUIRE (fullscreen.geometry == projection);
    // Native fullscreen composites retain source texture dimensions; using
    // projection dimensions here would change resolution uniforms/effects.
    REQUIRE (fullscreen.backing == texture);
}

TEST_CASE ("Native composite target extents follow source and model kind independently of the quad",
           "[scene][image][size][target]") {
    using WallpaperEngine::Render::Objects::loadedImageDimensions;
    const glm::vec2 source (150, 150);
    const glm::vec2 projection (3840, 2160);
    for (const auto logical : {glm::vec2 (1, 1), glm::vec2 (80, 70)}) {
        const auto manual = loadedImageDimensions (logical, {}, source, projection);
        REQUIRE (manual.geometry == logical);
        REQUIRE (manual.backing == source);
        const auto solid = loadedImageDimensions (logical, {.solidlayer = true}, source, projection);
        const auto passthrough = loadedImageDimensions (logical, {.passthrough = true}, source, projection);
        REQUIRE (solid.backing == glm::max (logical, glm::vec2 (4)));
        REQUIRE (passthrough.backing == solid.backing);
        const auto animated = loadedImageDimensions (logical, {.solidlayer = true, .animated = true},
                                                     source, projection);
        REQUIRE (animated.geometry == logical);
        REQUIRE (animated.backing == source);
    }
    // Native target helper1402edef0 rounds logical extents to nearest integer
    // with half away from zero before1401ea500 applies its minimum4.
    const auto rounded = loadedImageDimensions ({4.49f, 70.51f}, {.solidlayer = true},
                                                std::nullopt, projection);
    REQUIRE (rounded.geometry == glm::vec2 (4.49f, 70.51f));
    REQUIRE (rounded.backing == glm::vec2 (4, 71));
    const auto halves = loadedImageDimensions ({4.5f, 5.5f}, {.solidlayer = true},
                                               std::nullopt, projection);
    REQUIRE (halves.geometry == glm::vec2 (4.5f, 5.5f));
    REQUIRE (halves.backing == glm::vec2 (5, 6));
}

TEST_CASE ("Automatic projection preserves omitted image size independently of logical defaults",
           "[scene][image][size][projection]") {
    using WallpaperEngine::Render::Objects::imageAutoProjectionExtent;
    // Native and accepted rendering of an omitted-size autosize image with
    // a 500x291 source has a 500x291 canvas, not 501x292 from logical default1.
    REQUIRE (imageAutoProjectionExtent ({250, 145.5f}, std::nullopt) * 2.0f == glm::vec2 (500, 291));
    REQUIRE (imageAutoProjectionExtent ({-250, -145.5f}, std::nullopt) * 2.0f == glm::vec2 (500, 291));
    // Explicit sizes still contribute their authored extent, including zero
    // and one-axis values; they must not be treated as omitted constructor data.
    REQUIRE (imageAutoProjectionExtent ({100, 80}, glm::vec2 (80, 70)) == glm::vec2 (140, 115));
    REQUIRE (imageAutoProjectionExtent ({100, 80}, glm::vec2 (0, 70)) == glm::vec2 (100, 115));
    REQUIRE (imageAutoProjectionExtent ({100, 80}, glm::vec2 (0)) == glm::vec2 (100, 80));
}

TEST_CASE ("Autosize project layers retain scene dimensions when the root source resizes",
           "[scene][image][size][target][projection]") {
    using WallpaperEngine::Render::Objects::loadedImageDimensions;
    const glm::vec2 projection (3840, 2160);
    // Native1401fac50 sets projectlayer bit0x400;140209360 selects scene
    // dimensions for its nonanimated autosize path. A window-sized input must
    // not change the resolution of the authored full-scene filter chain.
    for (const auto source : {glm::vec2 (1280, 720), glm::vec2 (1000, 582)}) {
        const auto project = loadedImageDimensions ({3840, 2160},
            {.autosize = true, .passthrough = true, .projectlayer = true}, source, projection);
        REQUIRE (project.logical == projection);
        REQUIRE (project.geometry == projection);
        REQUIRE (project.backing == projection);
        const auto ordinary = loadedImageDimensions ({3840, 2160}, {.autosize = true}, source, projection);
        REQUIRE (ordinary.logical == source);
        REQUIRE (ordinary.geometry == source);
        REQUIRE (ordinary.backing == source);
    }
    // The native fullscreen and animation branches precede projectlayer.
    const glm::vec2 source (150, 150);
    const auto fullscreen = loadedImageDimensions ({80, 70},
        {.autosize = true, .fullscreen = true, .projectlayer = true}, source, projection);
    REQUIRE (fullscreen.logical == source);
    REQUIRE (fullscreen.geometry == projection);
    REQUIRE (fullscreen.backing == source);
    const auto animated = loadedImageDimensions ({80, 70},
        {.autosize = true, .passthrough = true, .animated = true, .projectlayer = true}, source, projection);
    REQUIRE (animated.logical == source);
    REQUIRE (animated.geometry == source);
    REQUIRE (animated.backing == source);
}
