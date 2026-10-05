#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Render/Wallpapers/VolumetricLight.h"
#include "WallpaperEngine/Scripting/ScriptPropertyBindings.h"

#include <cmath>

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
