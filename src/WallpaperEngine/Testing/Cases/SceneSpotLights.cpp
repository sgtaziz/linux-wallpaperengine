#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Data/Parsers/WallpaperParser.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Render/Wallpapers/SceneTransform.h"
#include "WallpaperEngine/Render/Wallpapers/SpotLightUniforms.h"
#include "WallpaperEngine/Render/Shaders/ShaderUnit.h"
#include "WallpaperEngine/Scripting/ScriptPropertyBindings.h"

#include <cmath>

using namespace WallpaperEngine::Data::Model;
using WallpaperEngine::Data::JSON::JSON;
using WallpaperEngine::Data::Parsers::ObjectParser;
using WallpaperEngine::Data::Parsers::WallpaperParser;
using WallpaperEngine::Assets::AssetLocator;
using WallpaperEngine::FileSystem::Container;

TEST_CASE ("Modern spotlight parses native cones and defaults independently of point lights", "[scene][spotlight]") {
    Project project {};
    auto object = ObjectParser::parse (JSON {{"id", 177}, {"light", "lspot"},
        {"origin", "1126.83899 745.13470 500"}, {"angles", "0 0 -1.58676"},
        {"color", "0.27059 0.25098 0.19216"}, {"controlpoint", "200 0 0"},
        {"innercone", 54.91f}, {"outercone", 64.519997f}, {"intensity", 2.0f},
        {"radius", 2048.0f}, {"castshadow", false}}, project);
    REQUIRE (object->is<SceneSpotLight> ());
    REQUIRE_FALSE (object->is<ScenePointLight> ());
    const auto& light = *object->as<SceneSpotLight> ();
    REQUIRE (light.innerCone->value->getFloat () == 54.91f);
    REQUIRE (light.outerCone->value->getFloat () == 64.519997f);
    REQUIRE (light.controlPoint->value->getVec3 () == glm::vec3 (200, 0, 0));
    REQUIRE_FALSE (light.castShadow);
    REQUIRE_FALSE (light.useCookie);
    const auto defaults = ObjectParser::parse (JSON {{"id", 1}, {"light", "lspot"}}, project);
    const auto& empty = *defaults->as<SceneSpotLight> ();
    REQUIRE (empty.color->value->getVec3 () == glm::vec3 (0));
    REQUIRE (empty.intensity->value->getFloat () == 0);
    REQUIRE (empty.radius->value->getFloat () == 1);
    REQUIRE (empty.exponent->value->getFloat () == 2);
    REQUIRE (empty.innerCone->value->getFloat () == 20);
    REQUIRE (empty.outerCone->value->getFloat () == 30);
    REQUIRE (empty.controlPoint->value->getVec3 () == glm::vec3 (0, 2, 0));
    const auto bindings = WallpaperEngine::Scripting::scriptPropertyBindings (light);
    const auto cone = std::ranges::find_if (bindings, [] (const auto& binding) {
        return std::string (binding.name) == "innercone";
    });
    REQUIRE (cone != bindings.end ());
    REQUIRE (&cone->value == light.innerCone->value.get ());
}

TEST_CASE ("Spotlight shader slots use the native general lightconfig field", "[scene][spotlight][parser]") {
    auto files = std::make_unique<Container> ();
    files->getVFS ().add ("scene.json", R"({
      "camera":{"center":"0 0 -1","eye":"0 0 0","up":"0 1 0"},
      "general":{"lightconfig":{"point":2,"spot":1}},
      "lightconfig":{"spot":9},"objects":[]})");
    Project project {};
    project.type = Project::Type_Scene;
    project.assetLocator = std::make_unique<AssetLocator> (std::move (files));
    const auto wallpaper = WallpaperParser::parse (JSON ("scene.json"), project);
    const auto* scene = wallpaper->as<Scene> ();
    REQUIRE (scene->pointLightSlots == 2);
    REQUIRE (scene->spotLightSlots == 1);
}

TEST_CASE ("Spotlight uses authored world column zero and degree cone cosines", "[scene][spotlight][geometry]") {
    using WallpaperEngine::Render::Wallpapers::packSpotLightUniforms;
    // Native 311 light177: unshadowed cone facing almost local -Y. The
    // direction producer takes +X of the node matrix; controlpoint is not
    // a second aim target in this plain-cone branch.
    const auto matrix = glm::translate (glm::mat4 (1), glm::vec3 (1126.83899f, 745.13470f, 500))
        * glm::rotate (glm::mat4 (1), -1.58676f, glm::vec3 (0, 0, 1));
    const auto packed = packSpotLightUniforms (matrix, {0.27059f, 0.25098f, 0.19216f},
                                               2, 2048, 2, 54.91f, 64.519997f);
    REQUIRE (std::abs (packed.color.x - 0.54118f) < 1e-6f);
    REQUIRE (packed.color.w == 2048);
    REQUIRE (packed.origin.x == 1126.83899f);
    REQUIRE (packed.origin.y == 745.13470f);
    REQUIRE (packed.origin.z == 500);
    REQUIRE (std::abs (packed.direction.x + 0.015962995f) < 1e-5f);
    REQUIRE (std::abs (packed.direction.y + 0.9998726f) < 1e-5f);
    REQUIRE (packed.direction.z == 0);
    REQUIRE (packed.origin.w > packed.direction.w);
    REQUIRE (std::abs (packed.origin.w - 0.57486254f) < 1e-5f);
    REQUIRE (std::abs (packed.direction.w - 0.43019688f) < 1e-5f);
    REQUIRE (packed.exponent == glm::vec4 (2, 0, 0, 0));
    // Native does not normalize the authored basis during uniform packing.
    const auto scaled = packSpotLightUniforms (matrix * glm::scale (glm::mat4 (1), glm::vec3 (3, 2, 1)),
                                               {1, 1, 1}, 1, 12, 3, 20, 30);
    REQUIRE (std::abs (glm::length (glm::vec3 (scaled.direction)) - 3) < 1e-5f);
    REQUIRE (scaled.origin.x == packed.origin.x);
}

TEST_CASE ("LightingV1 point and spotlight module links at native slot limits", "[scene][spotlight][shader]") {
    using WallpaperEngine::Render::Shaders::ShaderUnit;
    using WallpaperEngine::Render::Shaders::GLSLContext;
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    const ShaderConstantMap constants;
    const TextureMap textures;
    const ComboMap combos {{"LIGHTING", 1}};
    for (const int spots : {0, 1, 15}) {
        const ComboMap overrides {{"LIGHTS_POINT", 2}, {"LIGHTS_SPOT", spots}};
        ShaderUnit vertex (GLSLContext::UnitType_Vertex, "spots.vert",
            "attribute vec3 a_Position;\nvoid main() { gl_Position = vec4(a_Position, 1); }\n",
            assets, constants, textures, textures, combos, overrides);
        ShaderUnit fragment (GLSLContext::UnitType_Fragment, "spots.frag",
            "vec3 ComputePBRLightShadow(vec3 N, vec3 L, vec3 V, vec3 albedo, vec3 color, "
            "float radius, float exponent, vec3 specular, vec3 ambient, float roughness, "
            "float metallic, float shadow) { return color * radius * shadow; }\n"
            "#require LightingV1\n"
            "void main() { gl_FragColor = vec4(PerformLighting_V1(vec3(0),vec3(1),"
            "vec3(0,0,1),vec3(0,0,1),vec3(1),vec3(0),0.7,0),1); }\n",
            assets, constants, textures, textures, combos, overrides);
        const auto result = GLSLContext::get ().toGlsl (vertex.compile (), fragment.compile ());
        REQUIRE_FALSE (result.first.empty ());
        REQUIRE_FALSE (result.second.empty ());
    }
}
