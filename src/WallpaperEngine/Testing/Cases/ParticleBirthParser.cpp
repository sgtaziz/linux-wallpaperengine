#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Scripting/ParticleScriptBindings.h"
#include "WallpaperEngine/Render/Objects/ParticleInstancePatch.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <array>

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

TEST_CASE ("Birth remap CP translation outputs compile ownership for every native slot",
           "[particle][birth][parser][remap][controlpoint]") {
    Project project {};
    auto data = JSON::parse (R"({"id":1,"particle":{"controlpoint":[
        {"id":90,"offset":"7 11 13"},
        {"id":91,"offset":"17 19 23","flags":7,"angles":"30 40 50"}
    ],"initializer":[]}})");
    for (int index = 0; index < 8; ++index) {
        data["particle"]["initializer"].push_back (JSON {
            {"name", "remapinitialvalue"}, {"output", "controlpoint"},
            {"outputcontrolpoint0", index}, {"input", "size"},
            {"operation", "add"}, {"outputcomponent", "y"},
            {"outputrangemin", "2 3 5"}, {"outputrangemax", "7 11 13"}});
    }
    const auto object = ObjectParser::parse (data, project);
    const auto& particle = *object->as<Particle> ();
    REQUIRE (particle.initializers.size () == 8);
    REQUIRE (particle.controlPoints.size () == 8);
    for (int index = 0; index < 8; ++index) {
        const auto& remap = *particle.initializers[index]->as<RemapInitialValueInitializer> ()
            ->remap->as<VectorRemapValueOperator> ();
        REQUIRE (remap.output == VectorRemapValueOperator::Output::ControlPoint);
        REQUIRE (remap.outputControlPoint0 == index);
        REQUIRE (remap.operation == VectorRemapValueOperator::Operation::Add);
        REQUIRE (remap.outputComponent == VectorRemapValueOperator::OutputComponent::Y);
        REQUIRE (remap.outputMin == glm::vec3 (2.0f, 3.0f, 5.0f));
        REQUIRE (remap.outputMax == glm::vec3 (7.0f, 11.0f, 13.0f));
        const auto& cp = particle.controlPoints[index];
        REQUIRE (cp.id == index); // authored IDs do not select runtime slots
        REQUIRE ((cp.flags & 0x10000u) != 0);
        if (index > 1) REQUIRE (cp.offset == glm::vec3 (0.0f));
    }
    REQUIRE (particle.controlPoints[0].offset == glm::vec3 (7.0f, 11.0f, 13.0f));
    REQUIRE (particle.controlPoints[1].offset == glm::vec3 (17.0f, 19.0f, 23.0f));
    REQUIRE (particle.controlPoints[1].flags == 0x10007u);
    REQUIRE (particle.controlPoints[1].angles == glm::vec3 (30.0f, 40.0f, 50.0f));
}

TEST_CASE ("Birth CP output bounds literal targets while runtime and unsupported outputs stay gated",
           "[particle][birth][parser][remap][controlpoint]") {
    Project project {};
    const auto data = JSON::parse (R"({"id":1,"particle":{"initializer":[
        {"name":"remapinitialvalue","output":"controlpoint"},
        {"name":"remapinitialvalue","output":"controlpoint","outputcontrolpoint0":-1},
        {"name":"remapinitialvalue","output":"controlpoint","outputcontrolpoint0":8},
        {"name":"remapinitialvalue","output":"controlpoint","outputcontrolpoint0":2,
         "input":"controlpoint","inputcontrolpoint0":2,"operation":"multiply"},
        {"name":"remapinitialvalue","output":"controlpoint","outputcontrolpoint0":3,
         "input":"deltatocontrolpoint","inputcontrolpoint0":3},
        {"name":"remapinitialvalue","output":"controlpoint","outputcontrolpoint0":4,
         "input":"directiontocontrolpoint","inputcontrolpoint0":4},
        {"name":"remapinitialvalue","output":"controlpoint","outputcontrolpoint0":{"value":2}},
        {"name":"remapinitialvalue","output":"controlpoint","outputcontrolpoint0":5,
         "outputrangemin":[1,2,3]},
        {"name":"remapinitialvalue","output":"controlpoint","outputcontrolpoint0":6,
         "inputrangemax":[1,2,3]},
        {"name":"remapinitialvalue","output":"controlpoint","outputcontrolpoint0":3,
         "outputrangemax":{"value":"1 2 3","script":"rangeScript"}},
        {"name":"remapinitialvalue","output":"controlpoint","outputcontrolpoint1":2},
        {"name":"remapinitialvalue","output":"deltatocontrolpoint"},
        {"name":"remapinitialvalue","output":"directiontocontrolpoint"},
        {"name":"remapinitialvalue","output":"controlpoint","outputcomponent":"sum"},
        {"name":"remapinitialvalue","output":"size","outputcontrolpoint0":3}
    ],"operator":[
        {"name":"remapvalue","output":"controlpoint"},
        {"name":"remapvalue","output":"controlpoint","outputcontrolpoint0":5},
        {"name":"remapvalue","output":"color","outputcontrolpoint0":5},
        {"name":"remapvalue","input":"controlpoint","inputcontrolpoint0":2,"output":"position"}
    ]}})");
    const auto object = ObjectParser::parse (data, project);
    const auto& particle = *object->as<Particle> ();
    REQUIRE (particle.initializers.size () == 3);
    const std::array<int, 3> targets { 0, 7, 7 };
    for (size_t index = 0; index < targets.size (); ++index) {
        const auto& remap = *particle.initializers[index]->as<RemapInitialValueInitializer> ()
            ->remap->as<VectorRemapValueOperator> ();
        REQUIRE (remap.outputControlPoint0 == targets[index]);
    }
    REQUIRE (particle.controlPoints.size () == 2); // rejected input mutation does not claim ownership
    REQUIRE (particle.controlPoints[0].id == 0);
    REQUIRE (particle.controlPoints[1].id == 7);
    REQUIRE (particle.operators.size () == 1);
    REQUIRE (particle.operators[0]->as<VectorRemapValueOperator> ()->output
        == VectorRemapValueOperator::Output::Position);
}

TEST_CASE ("Sequence instance-count patch retains authored flag and node suppression",
           "[particle][birth][parser][sequence]") {
    Project project {};
    auto data = JSON::parse (R"({"id":1,"particle":{"initializer":[
        {"name":"mapsequencebetweencontrolpoints","flags":16},
        {"name":"mapsequencebetweencontrolpoints","flags":8}
    ]}})");
    auto object = ObjectParser::parse (data, project);
    REQUIRE (object->as<Particle> ()->initializers.size () == 2);
    REQUIRE (object->as<Particle> ()->initializers[0]
        ->as<MapSequenceBetweenControlPointsInitializer> ()->flags == 16u);
    data["particle"]["flags"] = 32;
    object = ObjectParser::parse (data, project);
    REQUIRE (object->as<Particle> ()->initializers.size () == 2);
    REQUIRE (object->as<Particle> ()->initializers[0]
        ->as<MapSequenceBetweenControlPointsInitializer> ()->flags == 16u);
}

TEST_CASE ("Root instance patch tracks writes rather than numerical changes",
           "[particle][birth][parser][sequence][instancepatch]") {
    using WallpaperEngine::Render::Objects::ParticleCore::InstancePatchWrites;
    using WallpaperEngine::Render::Objects::ParticleCore::InstanceSequencePatch;
    Project project {};
    const auto object = ObjectParser::parse (JSON::parse (R"({"id":1,"particle":{},
        "instanceoverride":{"count":{"value":1,"script":"export function update(v){return v;}"},
            "controlpoint1":"1 2 3","controlpointangle2":"4 5 6"}})"), project);
    const auto& instance = object->as<Particle> ()->instanceOverride;
    auto* count = instance.count->value.get ();
    REQUIRE (count->getScriptSource ().has_value ());
    {
        InstancePatchWrites writes (instance);
        REQUIRE_FALSE (writes.consume ());
        // Public instance.count uses update(float), including fractional writes
        // to an integer-authored seed. Script returns and user writes notify too.
        count->update (1.0f, DynamicValue::Script);
        REQUIRE (writes.consume ());
        REQUIRE_FALSE (writes.consume ());
        count->update (1.5f, DynamicValue::Script);
        REQUIRE (count->getFloat () == 1.5f);
        REQUIRE (writes.consume ());
        count->update (1.5f, DynamicValue::User);
        count->update (1.5f, DynamicValue::Script);
        REQUIRE (writes.consume ());
        REQUIRE_FALSE (writes.consume ()); // one outer flush batches writes

        for (auto* value : {instance.alpha->value.get (), instance.brightness->value.get (),
                           instance.size->value.get (), instance.speed->value.get (),
                           instance.lifetime->value.get ()}) {
            value->update (value->getFloat (), DynamicValue::Script);
            REQUIRE (writes.consume ());
        }
        instance.colorn->value->update (instance.colorn->value->getVec3 (), DynamicValue::Script);
        REQUIRE (writes.consume ());
        instance.controlPoints[1]->value->update (glm::vec3 (1, 2, 3), DynamicValue::Script);
        REQUIRE (writes.consume ());
        instance.controlPointAngles[2]->value->update (glm::vec3 (4, 5, 6), DynamicValue::User);
        REQUIRE (writes.consume ());
        instance.rate->value->update (2.0f, DynamicValue::Script);
        instance.enabled->value->update (false, DynamicValue::Script);
        instance.color->value->update (glm::vec3 (0), DynamicValue::Script);
        REQUIRE_FALSE (writes.consume ());
        writes.mark (); // native CP setter when no authored DynamicValue exists
        REQUIRE (writes.consume ());

        // A dirty write restores the positive step even at unchanged count.
        // The stream's literal is independent of a later initializer-count write.
        DynamicValue authored (5.0f);
        const InstanceSequencePatch patch {authored.getFloat (), true};
        authored.update (9.0f, DynamicValue::Script);
        float step = -0.25f;
        count->update (2.0f, DynamicValue::Script);
        if (writes.consume ()) patch.apply (step, count->getFloat ());
        REQUIRE (step == Catch::Approx (1.0f / 9.0f));
    }
    // The listener owner can retire while its model still receives updates.
    count->update (2.25f, DynamicValue::Script);
    REQUIRE (count->getFloat () == 2.25f);
    auto temporary = ObjectParser::parse (JSON::parse (R"({"id":2,"particle":{}})"), project);
    auto writes = std::make_unique<InstancePatchWrites> (temporary->as<Particle> ()->instanceOverride);
    temporary.reset (); // unsubscribe is safe when a DynamicValue dies first
    writes.reset ();
}
