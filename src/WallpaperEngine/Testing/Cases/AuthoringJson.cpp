#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/FileSystem/Adapters/Directory.h"
#include "WallpaperEngine/Logging/Log.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>

using WallpaperEngine::Data::JSON::JsonSyntaxError;
using WallpaperEngine::Data::JSON::parseAuthoringJson;

TEST_CASE ("Authored JSON accepts comments and trailing commas without changing strings", "[json]") {
    const auto value = parseAuthoringJson (R"({
      // top-level comment
      "url": "https://example.test/a//b/*c*/",
      "escaped": "quote: \" // still a string",
      "slashes": "\\/* not a comment */",
      "items": [1, /* between elements */ 2,],
    })", "fixture.json");

    REQUIRE (value["url"] == "https://example.test/a//b/*c*/");
    REQUIRE (value["escaped"] == "quote: \" // still a string");
    REQUIRE (value["slashes"] == "\\/* not a comment */");
    REQUIRE (value["items"].size () == 2);
    REQUIRE (value["items"][1] == 2);
}

TEST_CASE ("Authored JSON reports original source position for malformed input", "[json]") {
    try {
        (void)parseAuthoringJson ("{\n  \"items\": [1,,]\n}", "broken.json");
        FAIL ("Expected syntax error");
    } catch (const JsonSyntaxError& e) {
        REQUIRE (std::string (e.what ()).find ("broken.json:2:") != std::string::npos);
    }
    try {
        (void)parseAuthoringJson ("{\n  /* unterminated", "comment.json");
        FAIL ("Expected unterminated comment error");
    } catch (const JsonSyntaxError& e) {
        REQUIRE (std::string (e.what ()).find ("comment.json:2:3: unterminated block comment")
                 != std::string::npos);
    }
    try {
        (void)parseAuthoringJson ("{\"value\":1e10000}", "overflow.json");
        FAIL ("Expected numeric overflow error");
    } catch (const JsonSyntaxError& e) {
        REQUIRE (std::string (e.what ()).find ("overflow.json:") != std::string::npos);
        REQUIRE (std::string (e.what ()).find ("406") != std::string::npos);
    }
}

TEST_CASE ("Particle parser retains fractional warm-up and authored sphere sign", "[json][particle][parser]") {
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    const auto data = parseAuthoringJson (R"({
      "id": 9, "name": "sphere", "particle": {
        "starttime": 0.80000001,
        "emitter": [{"name": "sphererandom", "sign": "0 1 0",}],
      },
    })", "sphere.json");
    const auto object = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->startTime > 0.79f);
    REQUIRE (particle->startTime < 0.81f);
    REQUIRE (particle->emitters.size () == 1);
    REQUIRE (particle->emitters[0].sign.x == 0);
    REQUIRE (particle->emitters[0].sign.y == 1);
    REQUIRE (particle->emitters[0].sign.z == 0);
    REQUIRE (particle->emitters[0].flags == 1);
    REQUIRE_FALSE (particle->emitters[0].distanceMaxAuthored);
    auto explicitFlags = data;
    explicitFlags["particle"]["emitter"][0]["flags"] = 0;
    explicitFlags["particle"]["emitter"][0]["distancemax"] = "3 3 3";
    const auto explicitObject = ObjectParser::parse (explicitFlags, project);
    const auto* explicitParticle = dynamic_cast<const Particle*> (explicitObject.get ());
    REQUIRE (explicitParticle != nullptr);
    REQUIRE (explicitParticle->emitters[0].flags == 0);
    REQUIRE (explicitParticle->emitters[0].distanceMaxAuthored);

    auto invalid = data;
    invalid["particle"]["starttime"] = -1.0;
    REQUIRE_THROWS_AS (ObjectParser::parse (invalid, project), std::invalid_argument);
}

TEST_CASE ("Cursor-flower attraction keeps authored radii and native default flags",
           "[json][particle][parser][attract]") {
    using WallpaperEngine::Data::Model::ControlPointAttractOperator;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    const auto data = parseAuthoringJson (R"({
      "id": 27, "name": "cursor flower", "particle": {
        "operator": [
          {"name": "controlpointattract", "origin": "0 0 0", "scale": -600, "threshold": 50},
          {"name": "controlpointattract", "origin": "0 0 0", "scale": 500, "threshold": 5000},
          {"name": "controlpointattract", "offset": "3 4 0", "flags": 0}
        ]
      }
    })", "cursor-flower.json");
    const auto object = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->operators.size () == 3);
    const auto* inner = dynamic_cast<const ControlPointAttractOperator*> (
        particle->operators[0].get ());
    const auto* outer = dynamic_cast<const ControlPointAttractOperator*> (
        particle->operators[1].get ());
    const auto* explicitFlags = dynamic_cast<const ControlPointAttractOperator*> (
        particle->operators[2].get ());
    REQUIRE (inner != nullptr);
    REQUIRE (outer != nullptr);
    REQUIRE (explicitFlags != nullptr);
    REQUIRE (inner->flags == 2u);
    REQUIRE (inner->scale->value->getFloat () == -600.0f);
    REQUIRE (inner->threshold->value->getFloat () == 50.0f);
    REQUIRE (outer->flags == 2u);
    REQUIRE (outer->scale->value->getFloat () == 500.0f);
    REQUIRE (outer->threshold->value->getFloat () == 5000.0f);
    REQUIRE (explicitFlags->flags == 0u);
    REQUIRE (explicitFlags->origin->value->getVec3 () == glm::vec3 (3.0f, 4.0f, 0.0f));
}

TEST_CASE ("Sequence-around-control-point absent speed bounds preserve native zero defaults", "[json][particle][parser]") {
    using WallpaperEngine::Data::Model::MapSequenceAroundControlPointInitializer;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    const auto data = parseAuthoringJson (R"({
      "id": 29, "name": "sequence", "particle": {
        "initializer": [{"id": 4, "name": "mapsequencearoundcontrolpoint", "count": 1500,
                         "axis": "1 1 0"}]
      }
    })", "sequence-default.json");
    const auto parsed = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (parsed.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->initializers.size () == 1);
    const auto* sequence = dynamic_cast<const MapSequenceAroundControlPointInitializer*> (
        particle->initializers[0].get ());
    REQUIRE (sequence != nullptr);
    REQUIRE (sequence->speedMin->value->getVec3 () == glm::vec3 (0.0f));
    REQUIRE (sequence->speedMax->value->getVec3 () == glm::vec3 (0.0f));

    auto explicitBounds = data;
    explicitBounds["particle"]["initializer"][0]["speedmin"] = "1 2 3";
    explicitBounds["particle"]["initializer"][0]["speedmax"] = "4 5 6";
    const auto overridden = ObjectParser::parse (explicitBounds, project);
    const auto* explicitParticle = dynamic_cast<const Particle*> (overridden.get ());
    REQUIRE (explicitParticle != nullptr);
    const auto* explicitSequence = dynamic_cast<const MapSequenceAroundControlPointInitializer*> (
        explicitParticle->initializers[0].get ());
    REQUIRE (explicitSequence != nullptr);
    REQUIRE (explicitSequence->speedMin->value->getVec3 () == glm::vec3 (1.0f, 2.0f, 3.0f));
    REQUIRE (explicitSequence->speedMax->value->getVec3 () == glm::vec3 (4.0f, 5.0f, 6.0f));
}

TEST_CASE ("Particle instance brightness preserves authored and missing factors", "[json][particle][parser]") {
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    auto data = parseAuthoringJson (R"({
      "id": 59, "name": "bubble", "particle": {},
      "instanceoverride": {"brightness": 4.0, "alpha": 0.5}
    })", "brightness.json");
    const auto authored = ObjectParser::parse (data, project);
    const auto* bubble = dynamic_cast<const Particle*> (authored.get ());
    REQUIRE (bubble != nullptr);
    REQUIRE (bubble->instanceOverride.brightness->value->getFloat () == 4.0f);
    REQUIRE (bubble->instanceOverride.alpha->value->getFloat () == 0.5f);
    data["instanceoverride"].erase ("brightness");
    const auto missing = ObjectParser::parse (data, project);
    const auto* ordinary = dynamic_cast<const Particle*> (missing.get ());
    REQUIRE (ordinary != nullptr);
    REQUIRE (ordinary->instanceOverride.brightness->value->getFloat () == 1.0f);
}

TEST_CASE ("Scene camera object retains authored pose and FOV", "[json][camera][parser]") {
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::SceneCamera;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    const auto data = parseAuthoringJson (R"({
      "id": 87, "name": "", "camera": "default", "path": "scripts/camera_paths_87.json",
      "origin": "-0.30007 -0.04811 3.77126", "angles": "0.36399 -0.49698 0",
      "fov": 45, "zoom": 1
    })", "scene-camera.json");
    const auto object = ObjectParser::parse (data, project);
    const auto* camera = dynamic_cast<const SceneCamera*> (object.get ());
    REQUIRE (camera != nullptr);
    REQUIRE (camera->mode == "default");
    REQUIRE (camera->path == "scripts/camera_paths_87.json");
    REQUIRE (camera->origin->value->getVec3 () == glm::vec3 (-0.30007f, -0.04811f, 3.77126f));
    REQUIRE (camera->groupAngles->value->getVec3 () == glm::vec3 (0.36399f, -0.49698f, 0.0f));
    REQUIRE (camera->fov->value->getFloat () == 45.0f);
    REQUIRE (camera->zoom->value->getFloat () == 1.0f);
    REQUIRE (camera->groupVisible->value->getBool ());
}

TEST_CASE ("Size-random omitted bounds use native scene projection defaults", "[json][particle][parser]") {
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::SizeRandomInitializer;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    auto data = parseAuthoringJson (R"({
      "id":14,"name":"main","particle":{"initializer":[{"name":"sizerandom","min":1}]}
    })", "size-defaults.json");
    const auto parsed = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (parsed.get ());
    REQUIRE (particle != nullptr);
    const auto* size = dynamic_cast<const SizeRandomInitializer*> (particle->initializers[0].get ());
    REQUIRE (size != nullptr);
    REQUIRE (size->min->value->getFloat () == 1.0f);
    REQUIRE (size->max->value->getFloat () == 1.0f);

    project.sceneOrthogonalProjection = true;
    data["particle"]["initializer"][0].erase ("min");
    const auto orthogonal = ObjectParser::parse (data, project);
    const auto* orthoParticle = dynamic_cast<const Particle*> (orthogonal.get ());
    REQUIRE (orthoParticle != nullptr);
    const auto* orthoSize = dynamic_cast<const SizeRandomInitializer*> (
        orthoParticle->initializers[0].get ());
    REQUIRE (orthoSize != nullptr);
    REQUIRE (orthoSize->min->value->getFloat () == 5.0f);
    REQUIRE (orthoSize->max->value->getFloat () == 50.0f);

    data["particle"]["initializer"][0]["min"] = 2.0f;
    data["particle"]["initializer"][0]["max"] = 3.0f;
    const auto explicitBounds = ObjectParser::parse (data, project);
    const auto* explicitParticle = dynamic_cast<const Particle*> (explicitBounds.get ());
    REQUIRE (explicitParticle != nullptr);
    const auto* explicitSize = dynamic_cast<const SizeRandomInitializer*> (
        explicitParticle->initializers[0].get ());
    REQUIRE (explicitSize != nullptr);
    REQUIRE (explicitSize->min->value->getFloat () == 2.0f);
    REQUIRE (explicitSize->max->value->getFloat () == 3.0f);
}

TEST_CASE ("Alpha-random retains its authored exponent and native default", "[json][particle][parser]") {
    using WallpaperEngine::Data::Model::AlphaRandomInitializer;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    auto data = parseAuthoringJson (R"({
      "id": 12, "name": "glow", "particle": {"initializer": [
        {"name": "alpharandom", "min": 0.1, "max": 0.2, "exponent": 2}
      ]}
    })", "alpha-exponent.json");
    const auto authored = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (authored.get ());
    REQUIRE (particle != nullptr);
    const auto* alpha = dynamic_cast<const AlphaRandomInitializer*> (particle->initializers[0].get ());
    REQUIRE (alpha != nullptr);
    REQUIRE (alpha->exponent->value->getFloat () == 2.0f);
    data["particle"]["initializer"][0].erase ("exponent");
    const auto omitted = ObjectParser::parse (data, project);
    const auto* ordinary = dynamic_cast<const Particle*> (omitted.get ());
    REQUIRE (ordinary != nullptr);
    const auto* defaultAlpha = dynamic_cast<const AlphaRandomInitializer*> (ordinary->initializers[0].get ());
    REQUIRE (defaultAlpha != nullptr);
    REQUIRE (defaultAlpha->exponent->value->getFloat () == 1.0f);
}

TEST_CASE ("Layer-image emitter keeps native orthographic defaults and explicit overrides", "[json][particle][parser]") {
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    const auto data = parseAuthoringJson (R"({
      "id": 19, "name": "image-source", "particle": {
        "emitter": [{"id": 5, "name": "layerimage", "rate": 5000}]
      }
    })", "layerimage.json");
    const auto object = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->emitters.size () == 1);
    const auto& emitter = particle->emitters[0];
    REQUIRE (emitter.flags == 0x10000u);
    REQUIRE (emitter.speedMin == 0.1f);
    REQUIRE (emitter.speedMax == 0.2f);
    REQUIRE (emitter.offsetMin == glm::vec3 (-5.0f, -5.0f, 0.0f));
    REQUIRE (emitter.offsetMax == glm::vec3 (5.0f, 5.0f, 0.0f));

    auto explicitValues = data;
    auto& record = explicitValues["particle"]["emitter"][0];
    record["flags"] = 0;
    record["speedmin"] = 2.0f;
    record["offsetmin"] = "1 2 3";
    const auto overridden = ObjectParser::parse (explicitValues, project);
    const auto* explicitParticle = dynamic_cast<const Particle*> (overridden.get ());
    REQUIRE (explicitParticle != nullptr);
    REQUIRE (explicitParticle->emitters[0].flags == 0u);
    REQUIRE (explicitParticle->emitters[0].speedMin == 2.0f);
    REQUIRE (explicitParticle->emitters[0].offsetMin == glm::vec3 (1.0f, 2.0f, 3.0f));
}

TEST_CASE ("Image instance fields merge into the first material pass", "[json][image][parser]") {
    using WallpaperEngine::Assets::AssetLocator;
    using WallpaperEngine::Data::Model::Image;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::FileSystem::Container;
    auto files = std::make_unique<Container> ();
    files->getVFS ().add ("model.json", R"({"material":"material.json",})");
    files->getVFS ().add ("material.json", R"({
      // Source material slots are intentionally populated.
      "passes": [{"shader":"sprite", "textures":["base0","base1"],
                  "combos":{"BASE_ONLY":1,"SELECTED":0},
                  "constantshadervalues":{"base_only":0.25,"selected":0.1,"null_keeps":0.6,
                    "nested":{"value":0.3,
                      "script":"export function update(value) { return value; }",
                      "scriptproperties":{"factor":{"value":0.5,"script":"factor-script"}}}},
                  "usertextures":["userbase0","userbase1",
                    {"name":"$mediaThumbnail","type":"system","keepaspect":true},
                    {"name":"missing-type","keepaspect":false},
                    {"name":"unknown-type","type":"future","keepaspect":"yes"},
                    "userbase5"],},],
    })");
    Project project {};
    project.assetLocator = std::make_unique<AssetLocator> (std::move (files));
    const auto data = parseAuthoringJson (R"({
      "id": 10, "name": "image", "image": "model.json",
      "instance": {"textures":["instance0",null],
                   "combos":{"SELECTED":2,"ADDED":3,"BASE_ONLY":null},
                   "constantshadervalues":{"selected":0.8,"added":0.4,"null_keeps":null,
                     "nested":{"value":0.7,"scriptproperties":{"factor":{"value":0.9}}}},
                   "usertextures":["userinstance0",null,null,null,null,
                     {"name":"$mediaShortcut","type":"usershortcut","keepaspect":true}],},
    })", "scene.json");
    const auto object = ObjectParser::parse (data, project);
    const auto* image = dynamic_cast<const Image*> (object.get ());
    REQUIRE (image != nullptr);
    REQUIRE (image->model != nullptr);
    REQUIRE (image->model->material->passes.size () == 1);
    const auto& pass = *image->model->material->passes.front ();
    REQUIRE (pass.textures.at (0) == "instance0");
    REQUIRE (pass.textures.at (1) == "base1");
    REQUIRE (pass.combos.at ("BASE_ONLY") == 1);
    REQUIRE (pass.combos.at ("SELECTED") == 2);
    REQUIRE (pass.combos.at ("ADDED") == 3);
    REQUIRE (pass.constants.at ("base_only")->value->getFloat () == 0.25f);
    REQUIRE (pass.constants.at ("selected")->value->getFloat () == 0.8f);
    REQUIRE (pass.constants.at ("added")->value->getFloat () == 0.4f);
    REQUIRE (pass.constants.at ("null_keeps")->value->getFloat () == 0.6f);
    REQUIRE (pass.constants.at ("nested")->value->getFloat () == 0.7f);
    REQUIRE (pass.constants.at ("nested")->value->getScriptSource ()
             == "export function update(value) { return value; }");
    const auto& factor = pass.constants.at ("nested")->value->getProperties ().at ("factor");
    REQUIRE (factor->value->getFloat () == 0.9f);
    REQUIRE (factor->value->getScriptSource () == "factor-script");

    for (const auto& inert : {parseAuthoringJson ("null", "instance-null.json"),
                              parseAuthoringJson ("{}", "instance-empty.json"),
                              parseAuthoringJson ("\"unused\"", "instance-string.json")}) {
        auto inertData = data;
        inertData["instance"] = inert;
        const auto inertObject = ObjectParser::parse (inertData, project);
        const auto* inertImage = dynamic_cast<const Image*> (inertObject.get ());
        REQUIRE (inertImage != nullptr);
        const auto& inertPass = *inertImage->model->material->passes.front ();
        REQUIRE (inertPass.combos.at ("SELECTED") == 0);
        REQUIRE (inertPass.constants.at ("selected")->value->getFloat () == 0.1f);
    }
    REQUIRE (pass.usertextures.at (0).name == "userinstance0");
    REQUIRE (pass.usertextures.at (0).source == WallpaperEngine::Data::Model::UserTextureSource::Ordinary);
    REQUIRE_FALSE (pass.usertextures.at (0).keepAspect);
    REQUIRE (pass.usertextures.at (1).name == "userbase1");
    REQUIRE (pass.usertextures.at (1).source == WallpaperEngine::Data::Model::UserTextureSource::Ordinary);
    REQUIRE (pass.usertextures.at (2).name == "$mediaThumbnail");
    REQUIRE (pass.usertextures.at (2).source == WallpaperEngine::Data::Model::UserTextureSource::System);
    REQUIRE (pass.usertextures.at (2).keepAspect);
    REQUIRE (pass.usertextures.at (3).name == "missing-type");
    REQUIRE (pass.usertextures.at (3).source == WallpaperEngine::Data::Model::UserTextureSource::Ordinary);
    REQUIRE_FALSE (pass.usertextures.at (3).keepAspect);
    REQUIRE (pass.usertextures.at (4).name == "unknown-type");
    REQUIRE (pass.usertextures.at (4).source == WallpaperEngine::Data::Model::UserTextureSource::Ordinary);
    REQUIRE_FALSE (pass.usertextures.at (4).keepAspect);
    REQUIRE (pass.usertextures.at (5).name == "$mediaShortcut");
    REQUIRE (pass.usertextures.at (5).source == WallpaperEngine::Data::Model::UserTextureSource::UserShortcut);
    REQUIRE (pass.usertextures.at (5).keepAspect);
}

TEST_CASE ("Image animation layers retain authored blend controls", "[json][image][puppet]") {
    using WallpaperEngine::Assets::AssetLocator;
    using WallpaperEngine::Data::Model::Image;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::FileSystem::Container;
    auto files = std::make_unique<Container> ();
    files->getVFS ().add ("model.json", R"({"material":"material.json"})");
    files->getVFS ().add ("material.json", R"({"passes":[{"shader":"sprite"}]})");
    Project project {};
    project.assetLocator = std::make_unique<AssetLocator> (std::move (files));
    const auto data = parseAuthoringJson (R"({
      "id": 71, "name": "puppet", "image": "model.json",
      "animationlayers": [
        {"id": 2, "animation": 1425, "visible": true, "rate": 12.0,
         "blend": 0.25, "additive": false, "blendin": true,
         "blendout": false, "blendtime": 0.75},
        {"id": 3, "animation": 1750, "visible": true,
         "additive": true}
      ]
    })", "scene.json");
    const auto object = ObjectParser::parse (data, project);
    const auto* image = dynamic_cast<const Image*> (object.get ());
    REQUIRE (image != nullptr);
    REQUIRE (image->animationLayers.size () == 2);
    REQUIRE (image->animationLayers[0]->animation->value->getInt () == 1425);
    REQUIRE (image->animationLayers[0]->rate->value->getFloat () == 12.0f);
    REQUIRE (image->animationLayers[0]->blend->value->getFloat () == 0.25f);
    REQUIRE (image->animationLayers[0]->blendIn);
    REQUIRE_FALSE (image->animationLayers[0]->blendOut);
    REQUIRE (image->animationLayers[0]->blendTime == 0.75f);
    REQUIRE (image->animationLayers[1]->additive);
    REQUIRE (image->animationLayers[1]->blendTime == 0.5f);
}

TEST_CASE ("Image copybackground accepts literal and wrapped authoring values", "[json][image][parser]") {
    using WallpaperEngine::Assets::AssetLocator;
    using WallpaperEngine::Data::Model::Image;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::FileSystem::Container;
    auto files = std::make_unique<Container> ();
    files->getVFS ().add ("model.json", R"({"material":"material.json","passthrough":true})");
    files->getVFS ().add ("material.json", R"({"passes":[{"shader":"composelayer"}]})");
    Project project {};
    project.assetLocator = std::make_unique<AssetLocator> (std::move (files));
    auto data = parseAuthoringJson (
        R"({"id":11,"name":"composition","image":"model.json"})", "composition.json");
    auto parseValue = [&] () {
        auto object = ObjectParser::parse (data, project);
        const auto* image = dynamic_cast<const Image*> (object.get ());
        REQUIRE (image != nullptr);
        REQUIRE (image->copyBackground != nullptr);
        REQUIRE (image->copyBackground->value != nullptr);
        return image->copyBackground->value->getBool ();
    };
    REQUIRE (parseValue ());
    data["copybackground"] = false;
    REQUIRE_FALSE (parseValue ());
    data["copybackground"] = {{"value", false}};
    REQUIRE_FALSE (parseValue ());
    data["copybackground"] = {{"value", true}};
    REQUIRE (parseValue ());
}

TEST_CASE ("Authored text stays a string when it contains spaces or digits", "[text][parser]") {
    using WallpaperEngine::Data::Model::DynamicValue;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Text;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    for (const char* authored : {"D A Y", "12:34", "2010", "A B", "é"}) {
        auto input = parseAuthoringJson (R"({"id":42,"name":"text","text":{"value":"seed"}})", "text.json");
        input["text"]["value"] = authored;
        auto object = ObjectParser::parse (input, project);
        const auto* text = dynamic_cast<const Text*> (object.get ());
        REQUIRE (text != nullptr);
        REQUIRE (text->text->value->getType () == DynamicValue::String);
        REQUIRE (text->text->value->getString () == authored);
    }
}

TEST_CASE ("Text preserves authored parallax depth including a negative axis", "[text][parser][parallax]") {
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Text;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    auto input = parseAuthoringJson (R"({"id":42,"name":"text","text":"Clock",
      "parallaxDepth":"-0.10000 0.00000"})", "text-depth.json");
    auto object = ObjectParser::parse (input, project);
    const auto* text = dynamic_cast<const Text*> (object.get ());
    REQUIRE (text != nullptr);
    REQUIRE (text->parallaxDepth->value->getVec2 () == glm::vec2 (-0.1f, 0.0f));
    input.erase ("parallaxDepth");
    object = ObjectParser::parse (input, project);
    text = dynamic_cast<const Text*> (object.get ());
    REQUIRE (text != nullptr);
    REQUIRE (text->parallaxDepth->value->getVec2 () == glm::vec2 (0.0f));
}

TEST_CASE ("Unsupported particle components report object and JSON path", "[particle][parser]") {
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    static std::ostringstream errors;
    static bool registered = false;
    if (!registered) {
        sLog.addError (&errors);
        registered = true;
    }
    const size_t before = errors.str ().size ();
    Project project {};
    const auto input = parseAuthoringJson (R"({"id":42,"name":"unsupported",
      "particle":{"initializer":[{"name":"unrecognized-init"}],
                  "operator":[{"name":"unrecognized-op"}],
                  "renderer":[{"name":"unrecognized-renderer"}]}})", "unsupported.json");
    const auto object = ObjectParser::parse (input, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->initializers.empty ());
    REQUIRE (particle->operators.empty ());
    const std::string logged = errors.str ().substr (before);
    REQUIRE (logged.find ("object_id=42") != std::string::npos);
    REQUIRE (logged.find ("path=particle.initializer[0] name=unrecognized-init action=skipped")
             != std::string::npos);
    REQUIRE (logged.find ("path=particle.operator[0] name=unrecognized-op action=skipped")
             != std::string::npos);
    REQUIRE (logged.find ("path=particle.renderer[0] name=unrecognized-renderer action=sprite_fallback")
             != std::string::npos);
}

TEST_CASE ("Missing optional metadata retains a distinct filesystem error code", "[json][filesystem]") {
    using WallpaperEngine::Assets::AssetLoadException;
    using WallpaperEngine::Assets::AssetLocator;
    using WallpaperEngine::FileSystem::Container;
    AssetLocator assets (std::make_unique<Container> ());
    try {
        (void)assets.readString ("materials/absent.tex-json");
        FAIL ("Expected missing metadata error");
    } catch (const AssetLoadException& e) {
        REQUIRE (e.code () == std::errc::no_such_file_or_directory);
    }
}

TEST_CASE ("Directory adapter preserves non-absence errors for optional metadata", "[json][filesystem]") {
    namespace fs = std::filesystem;
    using WallpaperEngine::FileSystem::Adapters::DirectoryAdapter;
    const auto nonce = std::chrono::steady_clock::now ().time_since_epoch ().count ();
    const fs::path root = fs::temp_directory_path () / ("wpe-sidecar-fs-" + std::to_string (nonce));
    struct Cleanup {
        fs::path path;
        ~Cleanup () { std::error_code ignored; fs::remove_all (path, ignored); }
    } cleanup { root };
    fs::create_directory (root);
    fs::create_symlink ("loop", root / "loop");

    DirectoryAdapter adapter (root);
    REQUIRE_FALSE (adapter.exists ("missing.tex-json"));
    try {
        (void)adapter.exists ("loop");
        FAIL ("Expected a symlink-loop filesystem error");
    } catch (const fs::filesystem_error& e) {
        REQUIRE (e.code () == std::errc::too_many_symbolic_link_levels);
    }

    auto files = std::make_unique<WallpaperEngine::FileSystem::Container> ();
    files->mount (root, "/");
    WallpaperEngine::Assets::AssetLocator assets (std::move (files));
    try {
        (void)assets.readString ("loop");
        FAIL ("Expected the asset loader to preserve the filesystem error");
    } catch (const WallpaperEngine::Assets::AssetLoadException& e) {
        REQUIRE (e.code () == std::errc::too_many_symbolic_link_levels);
    }
}
