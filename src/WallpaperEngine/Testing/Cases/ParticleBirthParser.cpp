#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Scripting/ParticleScriptBindings.h"

#include <catch2/catch_test_macros.hpp>

using namespace WallpaperEngine::Data::Model;
using WallpaperEngine::Data::JSON::JSON;
using WallpaperEngine::Data::Parsers::ObjectParser;

TEST_CASE ("Birth initializer parser preserves native default and authored records",
           "[particle][birth][parser]") {
    Project project {};
    auto data = JSON::parse (R"({"id":1,"particle":{"initializer":[
        {"name":"mapsequencebetweencontrolpoints"},
        {"name":"mapsequencebetweencontrolpoints","count":10,"limitbehavior":"mirror",
         "sizereductionamount":0.25,"flags":32,"controlpointstart":2,"controlpointend":7},
        {"name":"positionoffsetrandom"},
        {"name":"positionoffsetrandom","scale":0,"distance":0,"timescale":0,
         "directions":"0 0 1","sign":"1 -1 0","octaves":1},
        {"name":"hsvcolorrandom"},
        {"name":"colorlist"},{"name":"colorlist","colors":[]},
        {"name":"colorlist","colors":["1 0 0","0 1 0"],"huenoise":0.25}
    ]}})");
    auto object = ObjectParser::parse (data, project);
    const auto* particle = object->as<Particle> ();
    REQUIRE (particle != nullptr);
    REQUIRE (particle->initializers.size () == 8);
    const auto* sequence = particle->initializers[0]->as<MapSequenceBetweenControlPointsInitializer> ();
    REQUIRE (sequence->count->value->getFloat () == 32.0f);
    REQUIRE (sequence->controlPointStart->value->getFloat () == 0.0f);
    REQUIRE (sequence->controlPointEnd->value->getFloat () == 1.0f);
    REQUIRE (sequence->bounds == glm::vec2 (0.0f, 1.0f));
    REQUIRE (sequence->arcAmount == 0.3f);
    REQUIRE (sequence->arcDirection == glm::vec3 (0.0f, 1.0f, 0.0f));
    REQUIRE (sequence->sizeReduction == 0.9f);
    const auto* authored = particle->initializers[1]->as<MapSequenceBetweenControlPointsInitializer> ();
    REQUIRE (authored->count->value->getFloat () == 10.0f);
    REQUIRE (authored->limitBehavior == "mirror");
    REQUIRE (authored->sizeReduction == 0.25f);
    REQUIRE (authored->flags == 32u);
    REQUIRE (authored->controlPointStart->value->getFloat () == 2.0f);
    const auto* offset = particle->initializers[2]->as<PositionOffsetRandomInitializer> ();
    // Project without scene camera uses the native perspective context.
    REQUIRE (offset->scale->value->getFloat () == 1.0f);
    REQUIRE (offset->distance->value->getFloat () == 0.1f);
    REQUIRE (offset->directions == glm::vec3 (1.0f));
    offset = particle->initializers[3]->as<PositionOffsetRandomInitializer> ();
    REQUIRE (offset->scale->value->getFloat () == 0.0f);
    REQUIRE (offset->distance->value->getFloat () == 0.0f);
    REQUIRE (offset->timeScale->value->getFloat () == 0.0f);
    REQUIRE (offset->directions == glm::vec3 (0.0f, 0.0f, 1.0f));
    REQUIRE (offset->sign == glm::vec3 (1.0f, -1.0f, 0.0f));
    const auto* hsv = particle->initializers[4]->as<HsvColorRandomInitializer> ();
    REQUIRE (hsv->hueSteps->value->getFloat () == 6.0f);
    REQUIRE (hsv->saturationMin->value->getFloat () == 0.5f);
    const auto* colors = particle->initializers[5]->as<ColorListInitializer> ();
    REQUIRE (colors->colors.size () == 1);
    REQUIRE (colors->colors[0]->value->getVec3 () == glm::vec3 (1.0f));
    REQUIRE (particle->initializers[6]->as<ColorListInitializer> ()->colors.empty ());
    colors = particle->initializers[7]->as<ColorListInitializer> ();
    REQUIRE (colors->colors[0]->value->getVec3 () == glm::vec3 (1.0f, 0.0f, 0.0f));
    REQUIRE (colors->colors[1]->value->getVec3 () == glm::vec3 (0.0f, 1.0f, 0.0f));
    REQUIRE (colors->hueNoise->value->getFloat () == 0.25f);
}

TEST_CASE ("Birth remap defaults and scalar outputs differ from live operators",
           "[particle][birth][parser][remap]") {
    Project project {};
    const auto data = JSON::parse (R"({"id":1,"particle":{"initializer":[
        {"name":"remapinitialvalue"},
        {"name":"remapinitialvalue","input":null},
        {"name":"remapinitialvalue","output":"maxlifetime","operation":"remap"},
        {"name":"remapinitialvalue","output":"rotation"},
        {"name":"remapinitialvalue","output":"angularspeed"},
        {"name":"remapinitialvalue","input":"position","output":"color",
         "inputcomponent":"y","outputrangemax":"1 0.5 0.25"}
    ],"operator":[{"name":"remapvalue"}]}})");
    const auto object = ObjectParser::parse (data, project);
    const auto* particle = object->as<Particle> ();
    REQUIRE (particle->initializers.size () == 6);
    const auto* defaulted = particle->initializers[0]->as<RemapInitialValueInitializer> ()
        ->remap->as<ScalarRemapValueOperator> ();
    REQUIRE (defaulted->input == ScalarRemapValueOperator::Input::MaxLifetime);
    REQUIRE (defaulted->output == ScalarRemapValueOperator::Output::Size);
    REQUIRE (defaulted->flags == 1);
    REQUIRE_FALSE (defaulted->blendEnvelope.has_value ());
    REQUIRE (particle->initializers[1]->as<RemapInitialValueInitializer> ()
        ->remap->as<ScalarRemapValueOperator> ()->input == ScalarRemapValueOperator::Input::MaxLifetime);
    const auto* lifetime = particle->initializers[2]->as<RemapInitialValueInitializer> ()
        ->remap->as<ScalarRemapValueOperator> ();
    REQUIRE (lifetime->output == ScalarRemapValueOperator::Output::MaxLifetime);
    REQUIRE (lifetime->operation == ScalarRemapValueOperator::Operation::Set);
    REQUIRE (particle->initializers[3]->as<RemapInitialValueInitializer> ()
        ->remap->as<ScalarRemapValueOperator> ()->output == ScalarRemapValueOperator::Output::Rotation);
    REQUIRE (particle->initializers[4]->as<RemapInitialValueInitializer> ()
        ->remap->as<ScalarRemapValueOperator> ()->output == ScalarRemapValueOperator::Output::AngularSpeed);
    const auto* vector = particle->initializers[5]->as<RemapInitialValueInitializer> ()
        ->remap->as<VectorRemapValueOperator> ();
    REQUIRE (vector->inputComponent == VectorRemapValueOperator::InputComponent::Y);
    REQUIRE (vector->outputMax == glm::vec3 (1.0f, 0.5f, 0.25f));
    REQUIRE (particle->operators[0]->as<ScalarRemapValueOperator> ()->input
        == ScalarRemapValueOperator::Input::LifetimeFraction);
}

TEST_CASE ("Extended birth color summary follows native authored order and fallback",
           "[particle][birth][parser][color]") {
    Project project {};
    auto data = JSON::parse (R"({"id":1,"particle":{"initializer":[
        {"name":"colorrandom","min":"255 255 255","max":"255 255 255"},
        {"name":"colorlist","colors":["1 0 0","0 1 0"]}
    ],"operator":[{"name":"colorchange","startvalue":"0 0 1"}]}})");
    auto object = ObjectParser::parse (data, project);
    REQUIRE (object->as<Particle> ()->presetHasColor);
    REQUIRE (object->as<Particle> ()->presetColorN == glm::vec3 (1.0f, 0.0f, 0.0f));
    data["particle"]["initializer"].push_back (JSON::parse (R"(
        {"name":"hsvcolorrandom","huemin":0.5,"huemax":0.5,
         "saturationmin":1,"saturationmax":1,"valuemin":1,"valuemax":1})"));
    object = ObjectParser::parse (data, project);
    REQUIRE (object->as<Particle> ()->presetColorN == glm::vec3 (0.0f, 1.0f, 1.0f));
    data["particle"]["initializer"] = JSON::array ({
        JSON::parse (R"({"name":"colorlist","colors":[]})")});
    object = ObjectParser::parse (data, project);
    REQUIRE (object->as<Particle> ()->presetHasColor);
    REQUIRE (object->as<Particle> ()->presetColorN == glm::vec3 (0.0f, 0.0f, 1.0f));
}

TEST_CASE ("New birth settings retain scripts for component lifecycle dispatch",
           "[particle][birth][parser][script]") {
    Project project {};
    const auto data = JSON::parse (R"({"id":1,"particle":{"initializer":[
        {"name":"positionoffsetrandom","distance":{"value":20,"script":"offsetScript"}},
        {"name":"mapsequencebetweencontrolpoints","count":{"value":10,"script":"countScript"}},
        {"name":"hsvcolorrandom","valuemin":{"value":0.5,"script":"valueScript"}},
        {"name":"colorlist","huenoise":{"value":0.1,"script":"noiseScript"}}
    ]}})");
    const auto object = ObjectParser::parse (data, project);
    std::map<std::string, std::string> scripts;
    WallpaperEngine::Scripting::forEachParticleScriptSetting (*object->as<Particle> (),
        [&] (const std::string& key, const DynamicValue& value) {
            if (value.getScriptSource ()) scripts[key] = *value.getScriptSource ();
        });
    REQUIRE (scripts.size () == 4);
    REQUIRE (scripts.at ("initializer0_distance") == "offsetScript");
    REQUIRE (scripts.at ("initializer1_count") == "countScript");
    REQUIRE (scripts.at ("initializer2_valueMin") == "valueScript");
    REQUIRE (scripts.at ("initializer3_hueNoise") == "noiseScript");
}

TEST_CASE ("Sequence instance-count patch is rejected unless native node flags suppress it",
           "[particle][birth][parser][sequence]") {
    Project project {};
    auto data = JSON::parse (R"({"id":1,"particle":{"initializer":[
        {"name":"mapsequencebetweencontrolpoints","flags":16},
        {"name":"mapsequencebetweencontrolpoints","flags":8}
    ]}})");
    auto object = ObjectParser::parse (data, project);
    REQUIRE (object->as<Particle> ()->initializers.size () == 1);
    REQUIRE (object->as<Particle> ()->initializers[0]
        ->as<MapSequenceBetweenControlPointsInitializer> ()->flags == 8u);
    data["particle"]["flags"] = 32;
    object = ObjectParser::parse (data, project);
    REQUIRE (object->as<Particle> ()->initializers.size () == 2);
    REQUIRE (object->as<Particle> ()->initializers[0]
        ->as<MapSequenceBetweenControlPointsInitializer> ()->flags == 16u);
}
