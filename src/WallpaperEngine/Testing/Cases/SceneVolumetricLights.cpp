#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Render/Wallpapers/SpotLightShadow.h"
#include "WallpaperEngine/Render/Wallpapers/VolumetricLight.h"
#include "WallpaperEngine/Scripting/ScriptPropertyBindings.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

using namespace WallpaperEngine::Data::Model;
using WallpaperEngine::Data::JSON::JSON;
using WallpaperEngine::Data::Parsers::ObjectParser;

TEST_CASE ("Point lights parse the native volumetric descriptors and defaults", "[scene][volumetric][parser]") {
    Project project {};
    // 3217188597 object 134.
    auto object = ObjectParser::parse (JSON::parse (R"({"id":134,"light":"lpoint","castvolumetrics":true,
        "density":1.74,"volumetricsexponent":1.0700001,"radius":500.0,"intensity":4.21})"), project);
    const auto& light = *object->as<ScenePointLight> ();
    REQUIRE (light.castVolumetrics->value->getBool ());
    REQUIRE (light.density->value->getFloat () == 1.74f);
    REQUIRE (light.volumetricsExponent->value->getFloat () == 1.0700001f);
    // registers the volumetric descriptors as script properties.
    const auto bindings = WallpaperEngine::Scripting::scriptPropertyBindings (light);
    for (const auto& [name, value] : {std::pair {"castvolumetrics", light.castVolumetrics->value.get ()},
                                      std::pair {"density", light.density->value.get ()},
                                      std::pair {"volumetricsexponent", light.volumetricsExponent->value.get ()}}) {
        const auto found = std::ranges::find_if (bindings, [&] (const auto& binding) { return std::string (binding.name) == name; });
        REQUIRE (found != bindings.end ());
        REQUIRE (&found->value == value);
    }
    // constructor: castvolumetrics off, density 2, volumetricsexponent 1.
    const auto defaults = ObjectParser::parse (JSON {{"id", 1}, {"light", "lpoint"}}, project);
    const auto& empty = *defaults->as<ScenePointLight> ();
    REQUIRE_FALSE (empty.castVolumetrics->value->getBool ());
    REQUIRE (empty.density->value->getFloat () == 2.0f);
    REQUIRE (empty.volumetricsExponent->value->getFloat () == 1.0f);
    // accepts {value, user}: the bit follows the user property.
    auto toggle = std::make_shared<PropertyBoolean> (PropertyData {"vol", "vol"}, true);
    project.properties["vol"] = toggle;
    const auto bound = ObjectParser::parse (JSON::parse (
        R"({"id":2,"light":"lpoint","castvolumetrics":{"user":"vol","value":false}})"), project);
    REQUIRE (bound->as<ScenePointLight> ()->castVolumetrics->value->getBool ());
    toggle->update (false, DynamicValue::User);
    REQUIRE_FALSE (bound->as<ScenePointLight> ()->castVolumetrics->value->getBool ());
}

TEST_CASE ("Point volume uniforms follow the native render-variable packing", "[scene][volumetric]") {
    using namespace WallpaperEngine::Render::Wallpapers;
    const glm::vec3 origin (-140.70251f, 934.66095f, 207.0f);
    const glm::vec3 color (0.96863f, 0.79216f, 0.57647f);
    const auto packed = packPointVolume (origin, color, 4.21f, 500.0f, 1.74f, 1.0700001f);
    // g_AltViewProjectionMatrix scales the unit volume mesh by the radius at the origin.
    REQUIRE (glm::length (glm::vec3 (packed.volume * glm::vec4 (0, 0, 1, 1)) - (origin + glm::vec3 (0, 0, 500))) < 1e-3f);
    REQUIRE (glm::vec3 (packed.volume[3]) == origin);
    // g_RenderVar1: the shader's radius is 0.99 of the volume's, intensity in w.
    REQUIRE (std::abs (packed.spotParams.x - 495.0f) < 1e-3f);
    REQUIRE (packed.spotParams.w == 4.21f);
    // g_RenderVar2 / g_RenderVar4: density beside the origin; the raw colour
    // (not scaled by intensity) beside volumetricsexponent.
    REQUIRE (packed.originDensity == glm::vec4 (origin, 1.74f));
    REQUIRE (packed.colorExponent == glm::vec4 (color, 1.0700001f));
}

TEST_CASE ("Volumetric buffers and the inside test follow ", "[scene][volumetric]") {
    using namespace WallpaperEngine::Render::Wallpapers;
    REQUIRE (volumetricBufferSize (1280, 720, 4) == glm::ivec2 (320, 180));
    REQUIRE (volumetricBufferSize (1281, 721, 2) == glm::ivec2 (160, 90));
    REQUIRE (volumetricBufferSize (3, 3, 4) == glm::ivec2 (1, 1));
    // 3217188597: the authored eye lies outside light 134's volume.
    const glm::vec3 eye (-438.78796f, 58.18182f, 0.0f);
    REQUIRE_FALSE (eyeInsidePointVolume (eye, glm::vec3 (0, 0, -1), glm::vec3 (-140.70251f, 934.66095f, 207.0f), 500.0f));
    // The 0.2 nudge along the view decides a boundary eye.
    REQUIRE (eyeInsidePointVolume (glm::vec3 (0, 0, 10.1f), glm::vec3 (0, 0, -1), glm::vec3 (0), 10.0f));
    REQUIRE_FALSE (eyeInsidePointVolume (glm::vec3 (0, 0, 10.3f), glm::vec3 (0, 0, -1), glm::vec3 (0), 10.0f));
    // light, invisible object, light, visible image -> one batch,
    // finished before the image only.
    REQUIRE_FALSE (volumetricBatchEndsBefore (true, true));
    REQUIRE_FALSE (volumetricBatchEndsBefore (false, false));
    REQUIRE (volumetricBatchEndsBefore (false, true));
}

TEST_CASE ("Modern spot and tube lights parse the volumetric descriptors and expose them to scripts",
           "[scene][volumetric][parser][spotlight][tubelight]") {
    Project project {};
    // registers castvolumetrics, density and volumetricsexponent for every modern light type.
    const auto spot = ObjectParser::parse (JSON::parse (R"({"id":3,"light":"lspot","castvolumetrics":true,
        "density":1.5,"volumetricsexponent":0.75,"radius":300.0,"innercone":12.0,"outercone":28.0})"), project);
    const auto& light = *spot->as<SceneSpotLight> ();
    REQUIRE (light.castVolumetrics->value->getBool ());
    REQUIRE (light.density->value->getFloat () == 1.5f);
    REQUIRE (light.volumetricsExponent->value->getFloat () == 0.75f);
    const auto tube = ObjectParser::parse (JSON::parse (R"({"id":4,"light":"ltube","castvolumetrics":true})"), project);
    REQUIRE (tube->as<SceneTubeLight> ()->castVolumetrics->value->getBool ());
    // constructor defaults: off, 2 and 1.
    for (const char* type : {"lspot", "ltube"}) {
        const auto defaults = ObjectParser::parse (JSON {{"id", 5}, {"light", type}}, project);
        const WallpaperEngine::Data::Model::SceneSpotLightData& empty = std::string (type) == "lspot"
            ? static_cast<const SceneSpotLightData&> (*defaults->as<SceneSpotLight> ())
            : static_cast<const SceneSpotLightData&> (*defaults->as<SceneTubeLight> ());
        REQUIRE_FALSE (empty.castVolumetrics->value->getBool ());
        REQUIRE (empty.density->value->getFloat () == 2.0f);
        REQUIRE (empty.volumetricsExponent->value->getFloat () == 1.0f);
    }
    const auto bindings = WallpaperEngine::Scripting::scriptPropertyBindings (light);
    for (const auto& [name, value] : {std::pair {"castvolumetrics", light.castVolumetrics->value.get ()},
                                      std::pair {"density", light.density->value.get ()},
                                      std::pair {"volumetricsexponent", light.volumetricsExponent->value.get ()}}) {
        const auto found = std::ranges::find_if (bindings, [&] (const auto& binding) { return std::string (binding.name) == name; });
        REQUIRE (found != bindings.end ());
        REQUIRE (&found->value == value);
    }
}

TEST_CASE ("Spot volume uniforms follow the native render-variable packing", "[scene][volumetric][spotlight]") {
    using namespace WallpaperEngine::Render::Wallpapers;
    // A spot at (100, 200, 300) whose node matrix column 0 is (0, -2, 0): the forward is not normalised.
    glm::mat4 world = glm::translate (glm::mat4 (1.0f), glm::vec3 (100.0f, 200.0f, 300.0f));
    world = glm::rotate (world, -glm::half_pi<float> (), glm::vec3 (0, 0, 1));
    world = glm::scale (world, glm::vec3 (2.0f));
    const auto projection = spotLightProjection (world, 25.0f, 450.0f, true);
    REQUIRE (projection.has_value ());
    const auto packed = packSpotVolume (world, *projection, {1.0f, 0.8f, 0.6f}, 3.0f, 450.0f, 10.0f, 25.0f, 2.0f, 1.5f);
    // g_RenderVar1: radius * 0.99, cos(inner), cos(outer), intensity.
    REQUIRE (packed.spotParams.x == Catch::Approx (445.5f));
    REQUIRE (packed.spotParams.y == Catch::Approx (std::cos (glm::radians (10.0f))));
    REQUIRE (packed.spotParams.z == Catch::Approx (std::cos (glm::radians (25.0f))));
    REQUIRE (packed.spotParams.w == 3.0f);
    // g_RenderVar2 / g_RenderVar4 as for points; g_RenderVar3 is the first node-matrix column.
    REQUIRE (packed.originDensity == glm::vec4 (100.0f, 200.0f, 300.0f, 2.0f));
    REQUIRE (packed.colorExponent == glm::vec4 (1.0f, 0.8f, 0.6f, 1.5f));
    REQUIRE (glm::length (glm::vec3 (packed.forward) - glm::vec3 (0.0f, -2.0f, 0.0f)) < 1e-5f);
    // g_AltViewProjectionMatrix is the inverse light projection, g_AltModelMatrix the projection.
    REQUIRE (packed.lightProjection == *projection);
    const glm::mat4 identity = packed.volume * *projection;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) REQUIRE (identity[c][r] == Catch::Approx (c == r ? 1.0f : 0.0f).margin (1e-3));
}

TEST_CASE ("The spot volume mesh is the native 32-segment light-NDC cylinder with fan caps", "[scene][volumetric][spotlight]") {
    using namespace WallpaperEngine::Render::Wallpapers;
    const auto mesh = buildSpotVolumeMesh (0.0f, 1.0f);
    REQUIRE (mesh.vertices.size () == 2 + 32 * 4);
    REQUIRE (mesh.indices.size () == 32 * 12);
    // the two centre vertices sit at (0.5, 0.5), near then far.
    REQUIRE (mesh.vertices[0] == glm::vec3 (0.5f, 0.5f, 1.0f));
    REQUIRE (mesh.vertices[1] == glm::vec3 (0.5f, 0.5f, 0.0f));
    for (const auto index : mesh.indices) REQUIRE (index < mesh.vertices.size ());
    for (size_t v = 2; v < mesh.vertices.size (); v += 4) {
        // far pair then near pair on the unit circle (angle i * 2pi / 32, y mirrored).
        const int segment = int ((v - 2) / 4);
        const float angle = float (segment) * glm::two_pi<float> () / 32.0f;
        REQUIRE (mesh.vertices[v].x == Catch::Approx (std::cos (angle)));
        REQUIRE (mesh.vertices[v].y == Catch::Approx (-std::sin (angle)));
        REQUIRE (mesh.vertices[v].z == 0.0f);
        REQUIRE (mesh.vertices[v + 2].z == 1.0f);
        REQUIRE (mesh.vertices[v + 2].x == mesh.vertices[v].x);
        REQUIRE (glm::length (glm::vec2 (mesh.vertices[v + 1])) == Catch::Approx (1.0f));
    }
    // Each segment: two side triangles, then one cap triangle on each centre vertex.
    REQUIRE (std::vector<uint32_t> (mesh.indices.begin (), mesh.indices.begin () + 12)
             == std::vector<uint32_t> {4, 2, 3, 4, 3, 5, 1, 3, 2, 0, 4, 5});
    // Through a spot projection the far-cap rim lies at the far distance with the cone's half-angle.
    glm::mat4 world = glm::translate (glm::mat4 (1.0f), glm::vec3 (10.0f, 20.0f, 30.0f));
    world = glm::rotate (world, glm::half_pi<float> (), glm::vec3 (0, 1, 0));
    const float radius = 400.0f, outer = 30.0f;
    const auto projection = *spotLightProjection (world, outer, radius, true);
    const glm::mat4 inverse = glm::inverse (projection);
    const glm::vec3 axis = glm::vec3 (world[0]);
    for (const int segment : {0, 8, 16, 24}) {
        const auto& rim = mesh.vertices[2 + 4 * segment];
        glm::vec4 world4 = inverse * glm::vec4 (rim.x * 0.99f, rim.y * 0.99f, rim.z, 1.0f);
        const glm::vec3 position = glm::vec3 (world4) / world4.w - glm::vec3 (world[3]);
        // The spot basis normalises with native's single Newton step (about 0.2% high), so compare at 0.5%.
        REQUIRE (glm::dot (position, axis) == Catch::Approx (radius).epsilon (5e-3));
        REQUIRE (glm::length (position - axis * glm::dot (position, axis))
                 == Catch::Approx (0.99f * radius * std::tan (glm::radians (outer))).epsilon (5e-3));
    }
}

TEST_CASE ("The eye-inside spot test follows the native cone predicate", "[scene][volumetric][spotlight]") {
    using namespace WallpaperEngine::Render::Wallpapers;
    // Apex (0, 0, 400), axis (0, 0, -1), outer 45 degrees, radius 800.
    glm::mat4 world = glm::translate (glm::mat4 (1.0f), glm::vec3 (0.0f, 0.0f, 400.0f));
    world = glm::rotate (world, glm::half_pi<float> (), glm::vec3 (0, 1, 0));
    const float radius = 800.0f;
    const auto inverse = glm::inverse (*spotLightProjection (world, 45.0f, radius, true));
    const glm::vec3 forward (0, 0, -1);
    const auto inside = [&] (glm::vec3 eye) { return eyeInsideSpotVolume (eye, forward, world, inverse, 0.0f, radius); };
    REQUIRE (inside ({0, 0, 0}));
    REQUIRE (inside ({300, 0, 0}));            // across 300 <= t (400.2) * tan 45
    REQUIRE_FALSE (inside ({420, 0, 0}));      // beyond the cone wall
    REQUIRE_FALSE (inside ({0, 0, 500}));      // behind the apex: t <= 0
    REQUIRE_FALSE (inside ({0, 0, -400.5f}));  // t = 800.7 > radius
    REQUIRE (inside ({0, 0, -399.0f}));        // t = 799.2 <= radius
}

TEST_CASE ("The full-screen volumetric march needs a perspective camera and the eye inside", "[scene][volumetric]") {
    using namespace WallpaperEngine::Render::Wallpapers;
    // Native replays: orthographic scenes always draw the volume mesh; perspective scenes with the eye inside draw volumetrics_fullscreen.
    REQUIRE (volumetricFullscreen (false, true));
    REQUIRE_FALSE (volumetricFullscreen (false, false));
    REQUIRE_FALSE (volumetricFullscreen (true, true));
    REQUIRE_FALSE (volumetricFullscreen (true, false));
}
