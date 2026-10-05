#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Data/Parsers/WallpaperParser.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Render/Wallpapers/SpotLightUniforms.h"
#include "WallpaperEngine/Render/Wallpapers/SpotLightShadow.h"
#include "WallpaperEngine/Render/Shaders/ShaderUnit.h"
#include "WallpaperEngine/Scripting/ScriptPropertyBindings.h"

#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

using namespace WallpaperEngine::Data::Model;
using WallpaperEngine::Data::JSON::JSON;
using WallpaperEngine::Data::Parsers::ObjectParser;
using WallpaperEngine::Data::Parsers::WallpaperParser;
using WallpaperEngine::Assets::AssetLocator;
using WallpaperEngine::FileSystem::Container;

TEST_CASE ("Modern tube light parses the shared native light descriptors", "[scene][tubelight]") {
    Project project {};
    // 3270865076 object 304.
    auto object = ObjectParser::parse (JSON {{"id", 304}, {"light", "ltube"},
        {"origin", "4030.12256 213.02130 250.00000"}, {"angles", "-0.00000 -0.00000 -0.07011"},
        {"color", "0.26275 0.00000 0.26275"}, {"controlpoint", "-1083.96838 2664.96729 0.00000"},
        {"intensity", 9.3400002f}, {"radius", 2048.0f}, {"exponent", 2.0f}, {"castshadow", false}}, project);
    REQUIRE (object->is<SceneTubeLight> ());
    REQUIRE_FALSE (object->is<SceneSpotLight> ());
    REQUIRE_FALSE (object->is<ScenePointLight> ());
    const auto& light = *object->as<SceneTubeLight> ();
    REQUIRE (light.controlPoint->value->getVec3 () == glm::vec3 (-1083.96838f, 2664.96729f, 0));
    REQUIRE (light.intensity->value->getFloat () == 9.3400002f);
    REQUIRE (light.radius->value->getFloat () == 2048.0f);
    const auto defaults = ObjectParser::parse (JSON {{"id", 1}, {"light", "ltube"}}, project);
    const auto& empty = *defaults->as<SceneTubeLight> ();
    REQUIRE (empty.radius->value->getFloat () == 1);
    REQUIRE (empty.exponent->value->getFloat () == 2);
    // the constructor default segment runs along local +X.
    REQUIRE (empty.controlPoint->value->getVec3 () == glm::vec3 (2, 0, 0));
    const auto bindings = WallpaperEngine::Scripting::scriptPropertyBindings (light);
    const auto control = std::ranges::find_if (bindings, [] (const auto& binding) {
        return std::string (binding.name) == "controlpoint";
    });
    REQUIRE (control != bindings.end ());
    REQUIRE (&control->value == light.controlPoint->value.get ());
}

TEST_CASE ("Tube shader slots use the native general lightconfig tube count", "[scene][tubelight][parser]") {
    const auto parse = [] (const std::string& config) {
        auto files = std::make_unique<Container> ();
        files->getVFS ().add ("scene.json", R"({"camera":{"center":"0 0 -1","eye":"0 0 0","up":"0 1 0"},
          "general":{"lightconfig":)" + config + R"(},"objects":[]})");
        Project project {};
        project.type = Project::Type_Scene;
        project.assetLocator = std::make_unique<AssetLocator> (std::move (files));
        return WallpaperParser::parse (JSON ("scene.json"), project);
    };
    const auto wallpaper = parse (R"({"tube":3})");
    REQUIRE (wallpaper->as<Scene> ()->tubeLightSlots == 3);
    REQUIRE (wallpaper->as<Scene> ()->spotLightSlots == 0);
    REQUIRE_THROWS (parse (R"({"tube":16})"));
}

TEST_CASE ("Tube light packs its segment from the authored node matrix", "[scene][tubelight][geometry]") {
    using WallpaperEngine::Render::Wallpapers::packTubeLightUniforms;
    const glm::vec3 origin (4030.12256f, 213.02130f, 250.0f);
    const glm::vec3 control (-1083.96838f, 2664.96729f, 0.0f);
    const auto matrix = glm::translate (glm::mat4 (1), origin)
        * glm::rotate (glm::mat4 (1), -0.07011f, glm::vec3 (0, 0, 1));
    const auto packed = packTubeLightUniforms (matrix, {0.26275f, 0, 0.26275f}, 9.3400002f, 2048, 2, control);
    REQUIRE (std::abs (packed.color.x - 0.26275f * 9.3400002f) < 1e-5f);
    REQUIRE (packed.color.y == 0);
    REQUIRE (packed.color.w == 2048);
    // Segment start: matrix translation, exponent in w.
    REQUIRE (glm::vec3 (packed.originA) == origin);
    REQUIRE (packed.originA.w == 2);
    // Segment end: the local control point through the full matrix, w = 0.
    const float c = std::cos (-0.07011f), s = std::sin (-0.07011f);
    const glm::vec3 end = origin + glm::vec3 (c * control.x - s * control.y, s * control.x + c * control.y, 0);
    REQUIRE (glm::length (glm::vec3 (packed.originB) - end) < 1e-2f);
    REQUIRE (packed.originB.w == 0);
    // A scaled node scales the segment as the matrix does.
    const auto scaled = packTubeLightUniforms (glm::scale (glm::mat4 (1), glm::vec3 (2)), {1, 1, 1}, 1, 1, 1, {1, 0, 0});
    REQUIRE (glm::vec3 (scaled.originB) == glm::vec3 (2, 0, 0));
}

TEST_CASE ("Tube lights fill serialized slots in native sort order", "[scene][tubelight]") {
    using namespace WallpaperEngine::Render::Wallpapers;
    const auto input = [] (int flags, float depth, float tag) {
        return TubeLightInput {flags, depth, {glm::vec4 (tag), glm::vec4 (0), glm::vec4 (0)}};
    };
    // Flags descend, then camera depth ascends; the fourth light has no slot.
    const auto packed = packTubeLights ({input (0, 5, 1), input (0, -1, 2), input (1, 9, 3), input (0, 7, 4)}, 3);
    REQUIRE (packed.size () == 3);
    REQUIRE (packed[0].color.x == 3);
    REQUIRE (packed[1].color.x == 2);
    REQUIRE (packed[2].color.x == 1);
    // Unused slots stay zero.
    const auto sparse = packTubeLights ({input (0, 0, 7)}, 2);
    REQUIRE (sparse[0].color.x == 7);
    REQUIRE (sparse[1].color == glm::vec4 (0));
    REQUIRE (packTubeLights ({input (0, 0, 7)}, 0).empty ());
}

TEST_CASE ("LightingV1 emits the native tube loop at the four-bit slot limits", "[scene][tubelight][shader]") {
    using WallpaperEngine::Render::Shaders::ShaderUnit;
    using WallpaperEngine::Render::Shaders::GLSLContext;
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    const ShaderConstantMap constants;
    const TextureMap textures;
    const ComboMap combos {{"LIGHTING", 1}};
    const std::string helpers =
        "vec3 PointSegmentDelta(vec3 pos, vec3 a, vec3 b) { return a - pos + b * 0.0; }\n"
        "vec3 ComputePBRLightShadow(vec3 N, vec3 L, vec3 V, vec3 albedo, vec3 color, "
        "float radius, float exponent, vec3 specular, vec3 ambient, float roughness, "
        "float metallic, float shadow) { return color * radius * exponent * shadow + L; }\n";
    for (const int tubes : {1, 3, 15}) {
        const ComboMap overrides {{"LIGHTS_POINT", 1}, {"LIGHTS_SPOT", 1}, {"LIGHTS_TUBE", tubes}};
        ShaderUnit vertex (GLSLContext::UnitType_Vertex, "tubes.vert",
            "attribute vec3 a_Position;\nvoid main() { gl_Position = vec4(a_Position, 1); }\n",
            assets, constants, textures, textures, combos, overrides);
        ShaderUnit fragment (GLSLContext::UnitType_Fragment, "tubes.frag",
            helpers + "#require LightingV1\n"
            "void main() { gl_FragColor = vec4(PerformLighting_V1(vec3(0),vec3(1),"
            "vec3(0,0,1),vec3(0,0,1),vec3(1),vec3(0),0.7,0),1); }\n",
            assets, constants, textures, textures, combos, overrides);
        const auto& source = fragment.compile ();
        // unshadowed, one block per slot after the last spot block.
        const std::string delta =
            "      vec3 lightDelta = PointSegmentDelta(worldPos, g_LTube_OriginA[i].xyz, g_LTube_OriginB[i].xyz);\n"
            "      light += ComputePBRLightShadow(normal, lightDelta, viewVector, color, g_LTube_Color[i].rgb, "
            "g_LTube_Color[i].w, g_LTube_OriginA[i].w, specularTint, ambient, roughness, metallic, 1.0); }\n";
        const auto lastSpot = source.rfind ("g_LSpot_Exponent[i].x");
        REQUIRE (lastSpot != std::string::npos);
        size_t cursor = lastSpot;
        for (int index = 0; index < tubes; ++index) {
            const auto block = source.find ("    { const uint i = " + std::to_string (index) + "u;\n" + delta, cursor);
            REQUIRE (block != std::string::npos);
            cursor = block + 1;
        }
        REQUIRE (source.find ("const uint i = " + std::to_string (tubes) + "u;\n      vec3 lightDelta = PointSegment")
                 == std::string::npos);
        REQUIRE (source.find ("uniform vec4 g_LTube_OriginB[" + std::to_string (tubes) + "];") != std::string::npos);
        const auto result = GLSLContext::get ().toGlsl (vertex.compile (), source);
        REQUIRE_FALSE (result.second.empty ());
    }
    const ComboMap tooMany {{"LIGHTS_TUBE", 16}};
    REQUIRE_THROWS (ShaderUnit (GLSLContext::UnitType_Fragment, "invalid.frag",
        helpers + "#require LightingV1\nvoid main() {}\n", assets, constants, textures, textures, combos, tooMany)
        .compile ());
}
