#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Data/Parsers/WallpaperParser.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Render/Wallpapers/SceneTransform.h"
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

TEST_CASE ("Generated lighting follows deferred helper declarations", "[scene][spotlight][shader]") {
    using namespace WallpaperEngine::Render::Shaders;
    auto files = std::make_unique<Container> ();
    files->getVFS ().add ("shaders/pbr.h",
        "vec3 ComputePBRLightShadow(vec3 N,vec3 L,vec3 V,vec3 albedo,vec3 color,"
        "float radius,float exponent,vec3 specular,vec3 ambient,float roughness,float metallic,float shadow)"
        "{return color * shadow + ambient * g_AmbientScale;}\n");
    AssetLocator assets (std::move (files));
    ShaderConstantMap constants;
    TextureMap textures;
    ComboMap combos {{"LIGHTING", 1}}, overrides {{"LIGHTS_SPOT", 1}};
    ShaderUnit vertex (GLSLContext::UnitType_Vertex, "deferred.vert",
        "attribute vec3 a_Position;\nvoid main(){gl_Position=vec4(a_Position,1);}\n",
        assets, constants, textures, textures, combos, overrides);
    ShaderUnit fragment (GLSLContext::UnitType_Fragment, "deferred.frag",
        "#include \"pbr.h\"\nuniform float g_AmbientScale;\n#require LightingV1\n"
        "void main(){gl_FragColor=vec4(PerformLighting_V1(vec3(0),vec3(1),"
        "vec3(0,0,1),vec3(0,0,1),vec3(1),vec3(0),0.7,0),1);}\n",
        assets, constants, textures, textures, combos, overrides);
    const auto linked = GLSLContext::get ().toGlsl (vertex.compile (), fragment.compile ());
    REQUIRE_FALSE (linked.first.empty ());
    REQUIRE_FALSE (linked.second.empty ());
}

TEST_CASE ("Spot sampling projection matches original reversed depth GPU constants",
           "[scene][spotlight][shadow]") {
    using namespace WallpaperEngine::Render::Wallpapers;
    const auto world = glm::translate (glm::mat4 (1), glm::vec3 (-34.9961357117f, 0, 160))
        * glm::rotate (glm::mat4 (1), glm::half_pi<float> (), glm::vec3 (0, 1, 0));
    const auto projection = spotLightProjection (world, 38, 360, false);
    REQUIRE (projection.has_value ());
    const float nativeRows[4][4] = {
        {1.2777761221f,0,-5.585337348e-8f,44.71723938f},
        {0,1.2777761221f,0,0},
        {6.061596537e-12f,0,0.0001386731601f,0.02781923674f},
        {-4.3637435e-8f,0,-0.9983081222f,159.7292938f}};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            REQUIRE (std::abs ((*projection)[column][row] - nativeRows[row][column]) < 2e-5f);
    const auto caster = spotLightCasterProjection (*projection);
    REQUIRE (std::abs (caster[3][2] - 0.02731923759f) < 1e-8f);
    const auto gl = spotLightGLCasterProjection (*projection);
    const glm::vec4 worldPoint (0, 20, 0, 1);
    const auto sampled = caster * worldPoint, raster = gl * worldPoint;
    REQUIRE (std::abs ((raster.z / raster.w + 1) * 0.5f - sampled.z / sampled.w) < 1e-6f);
    REQUIRE (std::abs ((raster.y / raster.w + 1) * 0.5f - (0.5f - sampled.y / sampled.w * 0.5f)) < 1e-6f);
    REQUIRE_FALSE (spotLightProjection (glm::mat4 (0), 38, 360, false));
}

TEST_CASE ("Spot feature subsets use the native two-bit lightconfig fields", "[scene][spotlight][shadow][parser]") {
    const auto parse = [] (const std::string& lightconfig) {
        auto files = std::make_unique<Container> ();
        files->getVFS ().add ("scene.json", R"({"camera":{"center":"0 0 -1","eye":"0 0 0","up":"0 1 0"},
          "general":{"lightconfig":)" + lightconfig + R"(},"objects":[]})");
        Project project {};
        project.type = Project::Type_Scene;
        project.assetLocator = std::make_unique<AssetLocator> (std::move (files));
        return WallpaperParser::parse (JSON ("scene.json"), project);
    };
    const auto wallpaper = parse (R"({"spot":4,"spotshadow":1,"spotcookie":1,"spotshadowcookie":2})");
    const auto* scene = wallpaper->as<Scene> ();
    REQUIRE (scene->spotLightSlots == 4);
    REQUIRE (scene->spotShadowSlots == 1);
    REQUIRE (scene->spotCookieSlots == 1);
    REQUIRE (scene->spotShadowCookieSlots == 2);
    // Subsets are leading slots of the total, never additional lights.
    REQUIRE_THROWS (parse (R"({"spot":1,"spotshadowcookie":1,"spotcookie":1})"));
    REQUIRE_THROWS (parse (R"({"spot":15,"spotshadow":4})"));
}

TEST_CASE ("Spot cookies and model casters retain native defaults", "[scene][spotlight][shadow][parser]") {
    Project project {};
    auto cast = std::make_shared<PropertyBoolean> (PropertyData {"cast", "cast"}, true);
    project.properties["cast"] = cast;
    const auto spot = ObjectParser::parse (JSON {{"id", 1}, {"light", "lspot"}, {"usecookie", true}}, project);
    REQUIRE (spot->as<SceneSpotLight> ()->cookie == "cookie/flashlight1");
    REQUIRE (spot->as<SceneSpotLight> ()->useCookie);
    const auto custom = ObjectParser::parse (JSON {{"id", 2}, {"light", "lspot"}, {"cookie", "cookie/round"}}, project);
    REQUIRE (custom->as<SceneSpotLight> ()->cookie == "cookie/round");
    const auto model = ObjectParser::parse (JSON {{"id", 3}, {"model", "models/a.mdl"}}, project);
    REQUIRE (model->as<SceneModel> ()->castShadow->value->getBool ());
    const auto disabled = ObjectParser::parse (
        JSON {{"id", 4}, {"model", "models/a.mdl"}, {"castshadow", false}}, project);
    REQUIRE_FALSE (disabled->as<SceneModel> ()->castShadow->value->getBool ());
    const auto bound = ObjectParser::parse (JSON::parse (
        R"({"id":5,"model":"models/a.mdl","castshadow":{"value":true,"user":"cast"}})"), project);
    REQUIRE (bound->as<SceneModel> ()->castShadow->value->getBool ());
    cast->update (false, DynamicValue::User);
    REQUIRE_FALSE (bound->as<SceneModel> ()->castShadow->value->getBool ());
}

TEST_CASE ("Spot lights take native category slot cursors", "[scene][spotlight][shadow]") {
    using namespace WallpaperEngine::Render::Wallpapers;
    // Generated loops order both, cookie, shadow, then plain spots.
    SpotLightSlotAllocator slots ({.total = 4, .shadow = 1, .cookie = 1, .shadowCookie = 1});
    REQUIRE (slots.next (spotLightCategory (true, false)) == 2);
    REQUIRE (slots.next (spotLightCategory (false, false)) == 3);
    REQUIRE (slots.next (spotLightCategory (true, true)) == 0);
    REQUIRE (slots.next (spotLightCategory (false, true)) == 1);
    REQUIRE_FALSE (slots.next (spotLightCategory (false, false)));
    // A category cursor continues into the following range; the total counter
    // still bounds the number of lights.
    SpotLightSlotAllocator overflow ({.total = 3, .shadow = 1, .cookie = 1, .shadowCookie = 0});
    REQUIRE (overflow.next (SpotLightCategory::Cookie) == 0);
    REQUIRE (overflow.next (SpotLightCategory::Cookie) == 1);
    REQUIRE (overflow.next (SpotLightCategory::Shadow) == 1);
    REQUIRE_FALSE (overflow.next (SpotLightCategory::Shadow));
}

TEST_CASE ("Spot lights sort by feature flags before camera depth", "[scene][spotlight][shadow]") {
    using namespace WallpaperEngine::Render::Wallpapers;
    struct Light { int id, flags; float depth; };
    // flags: castshadow bit0, usecookie bit1 (raw, before the quality mask).
    std::vector<Light> lights {{0, 0, -5}, {1, 1, 3}, {2, 3, 9}, {3, 2, 1}, {4, 1, -2}, {5, 1, 3}};
    std::stable_sort (lights.begin (), lights.end (), [] (const Light& left, const Light& right) {
        return spotLightPrecedes (left.flags, left.depth, right.flags, right.depth);
    });
    std::vector<int> order;
    for (const auto& light : lights) order.push_back (light.id);
    // Both, cookie, shadow (nearer first, equal depth keeps scene order), plain.
    REQUIRE (order == std::vector<int> {2, 3, 4, 1, 5, 0});
}

TEST_CASE ("Shadow atlas tiles follow the native shelf allocator", "[scene][spotlight][shadow]") {
    using namespace WallpaperEngine::Render::Wallpapers;
    REQUIRE (shadowAtlasTileSize (1) == 256);
    REQUIRE (shadowAtlasTileSize (2) == 256);
    REQUIRE (shadowAtlasTileSize (3) == 512);
    REQUIRE (shadowAtlasTileSize (4) == 1024);
    // The four original controls allocate one D32 1024 square.
    const auto single = packShadowAtlas ({1024});
    REQUIRE (single.width == 1024);
    REQUIRE (single.height == 1024);
    REQUIRE (shadowAtlasTransform (single.tiles[0], 1024, 1024) == glm::vec4 (0, 0, 1, 1));
    const auto two = packShadowAtlas ({1024, 1024});
    REQUIRE (two.width == 2048);
    REQUIRE (two.height == 1024);
    REQUIRE (two.tiles[1].x == 1024);
    REQUIRE (two.tiles[1].y == 0);
    REQUIRE (shadowAtlasTransform (two.tiles[1], 2048, 1024) == glm::vec4 (0.5f, 0, 0.5f, 1));
    // An 8192-wide shelf holds eight 1024 tiles; the ninth opens a new shelf.
    const auto nine = packShadowAtlas (std::vector<uint32_t> (9, 1024));
    REQUIRE (nine.width == 8192);
    REQUIRE (nine.height == 2048);
    REQUIRE (nine.tiles[8].x == 0);
    REQUIRE (nine.tiles[8].y == 1024);
    // Larger requests are placed first; a smaller one fills the same shelf.
    const auto mixed = packShadowAtlas ({256, 1024});
    REQUIRE (mixed.tiles[1].x == 0);
    REQUIRE (mixed.tiles[0].x == 1024);
    REQUIRE (mixed.tiles[0].y == 0);
    REQUIRE (mixed.width == 1280);
    REQUIRE (mixed.height == 1024);
}

TEST_CASE ("Unconditional system-value declarations lower to GLSL 330 builtins", "[scene][spotlight][shadow][shader]") {
    using namespace WallpaperEngine::Render::Shaders;
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    ShaderConstantMap constants;
    TextureMap textures;
    ComboMap combos;
    // Shipped shadowcaster.vert declares HLSL system values and selects a
    // viewport per instance. A guarded declaration is left to its backend.
    ShaderUnit vertex (GLSLContext::UnitType_Vertex, "caster.vert",
        "uniform mat4 g_Matrices[6];\nin uint gl_InstanceID;\nvarying uint gl_ViewportIndex;\n"
        "#if HLSL\nin uint gl_VertexID;\n#endif\nattribute vec3 a_Position;\n"
        "void main() { int v = gl_VertexID; gl_Position = mul(vec4(a_Position, float(v)), "
        "g_Matrices[gl_InstanceID]); gl_ViewportIndex = gl_InstanceID; }\n",
        assets, constants, textures, textures, combos, combos);
    ShaderUnit fragment (GLSLContext::UnitType_Fragment, "caster.frag", "void main() {}\n",
        assets, constants, textures, textures, combos, combos);
    vertex.linkToUnit (&fragment);
    fragment.linkToUnit (&vertex);
    const auto& compiled = vertex.compile ();
    REQUIRE (compiled.find ("g_Matrices[uint(gl_InstanceID)]") != std::string::npos);
    REQUIRE (compiled.find ("_wpe_gl_ViewportIndex = uint(gl_InstanceID);") != std::string::npos);
    REQUIRE (compiled.find ("#if HLSL\nin uint gl_VertexID;") != std::string::npos);
    const auto linked = GLSLContext::get ().toGlsl (compiled, fragment.compile ());
    REQUIRE_FALSE (linked.first.empty ());
    REQUIRE_FALSE (linked.second.empty ());
}

TEST_CASE ("Spot producer packs sorted slots, the last sorted cookie and atlas tiles", "[scene][spotlight][shadow]") {
    using namespace WallpaperEngine::Render::Wallpapers;
    const auto light = [] (bool shadow, bool cookie, float depth, int id) {
        SpotLightInput input;
        input.castShadow = shadow;
        input.useCookie = cookie;
        input.depth = depth;
        input.uniforms.color = glm::vec4 (float (id));
        input.projection = glm::mat4 (float (id + 1));
        return input;
    };
    // Scene order disagrees with both the category order and camera depth.
    const std::vector<SpotLightInput> lights {
        light (false, false, 0, 0), light (true, false, 5, 1), light (false, true, 2, 2),
        light (true, true, 1, 3), light (true, false, -3, 4)};
    const SpotLightSlotAllocator::Counts counts {.total = 5, .shadow = 2, .cookie = 1, .shadowCookie = 1};
    const auto packed = packSpotLights (lights, counts, 4, 0, 0);
    // both -> slot 0, cookie -> 1, shadow near then far -> 2, 3, plain -> 4.
    const std::vector<float> slotIds {3, 2, 4, 1, 0};
    for (size_t slot = 0; slot < slotIds.size (); ++slot) REQUIRE (packed.slots[slot].color.x == slotIds[slot]);
    // One cookie alias: the last cookie light in sorted order, not in scene order.
    REQUIRE (packed.cookie == 2);
    // A cookie-only light has a projection but no atlas tile.
    REQUIRE (packed.projections[1] == glm::mat4 (3.0f));
    REQUIRE (packed.transforms[1] == glm::vec4 (0.0f));
    REQUIRE (packed.views.size () == 3);
    REQUIRE (packed.atlasWidth == 3072);
    REQUIRE (packed.atlasHeight == 1024);
    const std::vector<int> viewSlots {0, 2, 3};
    for (size_t view = 0; view < 3; ++view) {
        REQUIRE (packed.views[view].slot == viewSlots[view]);
        REQUIRE (packed.views[view].tile.x == view * 1024);
        REQUIRE (packed.transforms[viewSlots[view]] == glm::vec4 (float (view) / 3, 0, 1.0f / 3, 1));
        REQUIRE (packed.views[view].casterProjection == spotLightGLCasterProjection (packed.projections[viewSlots[view]]));
    }
    // The allocated atlas only grows; transforms divide by its real size.
    const auto grown = packSpotLights (lights, counts, 4, 4096, 1024);
    REQUIRE (grown.atlasWidth == 4096);
    REQUIRE (grown.transforms[2] == glm::vec4 (0.25f, 0, 0.25f, 1));
    // The sort uses the live view forward. A camera on +X looking at the
    // origin puts the light at x=50 first despite its later scene order.
    const auto view = glm::lookAt (glm::vec3 (200, 0, 0), glm::vec3 (0), glm::vec3 (0, 1, 0));
    const auto forward = viewForward (view);
    REQUIRE (glm::length (forward - glm::vec3 (-1, 0, 0)) < 1e-6f);
    auto far = light (true, false, glm::dot (glm::vec3 (-50, 0, 0), forward), 7);
    auto near = light (true, false, glm::dot (glm::vec3 (50, 0, 0), forward), 8);
    const auto sideways = packSpotLights ({far, near}, {.total = 2, .shadow = 2, .cookie = 0, .shadowCookie = 0}, 4, 0, 0);
    REQUIRE (sideways.slots[0].color.x == 8);
    REQUIRE (sideways.slots[1].color.x == 7);
    REQUIRE (sideways.views[0].slot == 0);
    REQUIRE (sideways.views[0].casterProjection == spotLightGLCasterProjection (glm::mat4 (9.0f)));
    // A -Z camera keeps the scene order for these two lights.
    const auto front = viewForward (glm::lookAt (glm::vec3 (0, 0, 220), glm::vec3 (0), glm::vec3 (0, 1, 0)));
    far.depth = glm::dot (glm::vec3 (-50, 0, 0), front);
    near.depth = glm::dot (glm::vec3 (50, 0, 0), front);
    REQUIRE (packSpotLights ({far, near}, {.total = 2, .shadow = 2, .cookie = 0, .shadowCookie = 0}, 4, 0, 0)
                 .slots[0].color.x == 7);
    // Without shadow quality castshadow is ignored: no tiles are requested.
    const auto unshadowed = packSpotLights (lights, counts, 0, 0, 0);
    REQUIRE (unshadowed.views.empty ());
    REQUIRE (unshadowed.atlasWidth == 0);
}
