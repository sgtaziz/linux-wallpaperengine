#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Scripting/ParticleScriptBindings.h"
#include <catch2/catch_test_macros.hpp>
#include <set>
#include "WallpaperEngine/Render/Wallpapers/ParticleSceneClock.h"
#include "WallpaperEngine/Render/Objects/ParticleEventInheritance.h"
#include <catch2/catch_approx.hpp>

using namespace WallpaperEngine::Data::Model;
using WallpaperEngine::Data::JSON::JSON;
using WallpaperEngine::Data::Parsers::ObjectParser;

TEST_CASE ("Preset control point slots follow array order rather than editor IDs",
           "[particle][controlpoint][parser]") {
    // Native 1401c5490 at 1401d0530 reads array[index] and writes the
    // descriptor at index*0x20. A sparse {id:6},{id:7} fixture therefore
    // populates CP0/CP1, independently of scene instance override slots.
    Project project {};
    const auto object = ObjectParser::parse (JSON::parse (R"({"id":947,
      "instanceoverride":{"controlpoint1":"99 100 101"},
      "particle":{"controlpoint":[
        {"id":6,"offset":"17 18 19","flags":4,"parentcontrolpoint":5},
        {"id":7,"offset":"80 81 82","angles":"0.1 0.2 0.3"},
        {"id":"editor-only","offset":"7 8 9"},
        {"offset":"10 11 12"}
      ]}})"), project);
    REQUIRE (object != nullptr);
    REQUIRE (object->id == 947);
    const auto* particle = object->as<Particle> ();
    REQUIRE (particle->controlPoints.size () == 4);
    for (int index = 0; index < 4; ++index)
        REQUIRE (particle->controlPoints[index].id == index);
    REQUIRE (particle->controlPoints[0].offset == glm::vec3 (17, 18, 19));
    REQUIRE (particle->controlPoints[0].flags == 4);
    REQUIRE (particle->controlPoints[0].parentControlPoint == 5);
    REQUIRE (particle->controlPoints[1].offset == glm::vec3 (80, 81, 82));
    REQUIRE (particle->controlPoints[1].angles == glm::vec3 (0.1f, 0.2f, 0.3f));
    REQUIRE (particle->controlPoints[2].offset == glm::vec3 (7, 8, 9));
    REQUIRE (particle->controlPoints[3].offset == glm::vec3 (10, 11, 12));
    REQUIRE (particle->instanceOverride.controlPoints[0] == nullptr);
    REQUIRE (particle->instanceOverride.controlPoints[1]->value->getVec3 () == glm::vec3 (99, 100, 101));
    REQUIRE (particle->instanceOverride.controlPoints[2] == nullptr);
}

TEST_CASE ("Preset control point holes preserve slots and records beyond eight are ignored",
           "[particle][controlpoint][parser]") {
    Project project {};
    const auto object = ObjectParser::parse (JSON::parse (R"({"id":1,"particle":{
      "controlpoint":[null,42,"unused",[],false,
        {"id":0,"offset":"5 6 7"},null,
        {"id":-100,"offset":"70 71 72","flags":2},
        {"id":0,"offset":"900 901 902","flags":"must not be parsed"}
      ]}})"), project);
    REQUIRE (object != nullptr);
    const auto* particle = object->as<Particle> ();
    REQUIRE (particle->controlPoints.size () == 2);
    REQUIRE (particle->controlPoints[0].id == 5);
    REQUIRE (particle->controlPoints[0].offset == glm::vec3 (5, 6, 7));
    REQUIRE (particle->controlPoints[1].id == 7);
    REQUIRE (particle->controlPoints[1].offset == glm::vec3 (70, 71, 72));
    REQUIRE (particle->controlPoints[1].flags == 2);
}

TEST_CASE ("Control point component parser preserves projection defaults and authored zeroes", "[particle][controlpoint][parser]") {
    for (bool orthogonal : {false, true}) {
        Project project {};
        project.sceneOrthogonalProjection = orthogonal;
        const auto object = ObjectParser::parse (JSON::parse (R"({"id":1,"particle":{
            "initializer":[{"name":"inheritcontrolpointvelocity"},
              {"name":"inheritcontrolpointvelocity","min":0,"max":0,"controlpoint":7}],
            "operator":[{"name":"maintaindistancetocontrolpoint"},
              {"name":"maintaindistancetocontrolpoint","controlpoint":3,"distance":0,"variablestrength":0},
              {"name":"maintaindistancebetweencontrolpoints","controlpointstart":2,"controlpointend":5},
              {"name":"reducemovementnearcontrolpoint"},
              {"name":"reducemovementnearcontrolpoint","distanceinner":0,"distanceouter":0,
               "reductioninner":0,"reductionouter":0,"blendinend":0.2}]
        }})"), project);
        const auto* p = object->as<Particle> ();
        REQUIRE (p->initializers.size () == 2);
        REQUIRE (p->operators.size () == 5);
        const auto* velocity = p->initializers[0]->as<InheritControlPointVelocityInitializer> ();
        REQUIRE (velocity->min->value->getFloat () == 0.1f);
        REQUIRE (velocity->max->value->getFloat () == 0.2f);
        REQUIRE (velocity->controlPoint->value->getFloat () == 0);
        velocity = p->initializers[1]->as<InheritControlPointVelocityInitializer> ();
        REQUIRE (velocity->min->value->getFloat () == 0);
        REQUIRE (velocity->max->value->getFloat () == 0);
        REQUIRE (velocity->controlPoint->value->getFloat () == 7);
        const auto* distance = p->operators[0]->as<MaintainDistanceToControlPointOperator> ();
        REQUIRE (distance->distance->value->getFloat () == (orthogonal ? 200.f : 1.f));
        distance = p->operators[1]->as<MaintainDistanceToControlPointOperator> ();
        REQUIRE (distance->controlPoint->value->getFloat () == 3);
        REQUIRE (distance->distance->value->getFloat () == 0);
        REQUIRE (distance->variableStrength->value->getFloat () == 0);
        const auto* between = p->operators[2]->as<MaintainDistanceBetweenControlPointsOperator> ();
        REQUIRE (between->controlPointStart->value->getFloat () == 2);
        REQUIRE (between->controlPointEnd->value->getFloat () == 5);
        const auto* reduction = p->operators[3]->as<ReduceMovementNearControlPointOperator> ();
        REQUIRE (reduction->distanceInner->value->getFloat () == (orthogonal ? 100.f : .5f));
        REQUIRE (reduction->distanceOuter->value->getFloat () == (orthogonal ? 350.f : 1.f));
        REQUIRE (reduction->reductionInner->value->getFloat () == 100.f);
        reduction = p->operators[4]->as<ReduceMovementNearControlPointOperator> ();
        REQUIRE (reduction->distanceInner->value->getFloat () == 0);
        REQUIRE (reduction->distanceOuter->value->getFloat () == 0);
        REQUIRE (reduction->reductionInner->value->getFloat () == 0);
        REQUIRE (reduction->reductionOuter->value->getFloat () == 0);
        REQUIRE (reduction->blendEnvelope->inEnd->value->getFloat () == .2f);
        std::set<std::string> visited;
        WallpaperEngine::Scripting::forEachParticleScriptSetting (*p,
            [&] (const std::string& name, auto&) { visited.insert (name); });
        REQUIRE (visited.contains ("initializer1_controlPoint"));
        REQUIRE (visited.contains ("initializer1_min"));
        REQUIRE (visited.contains ("operator1_variableStrength"));
        REQUIRE (visited.contains ("operator2_controlPointEnd"));
        REQUIRE (visited.contains ("operator4_distanceInner"));
        REQUIRE (visited.contains ("operator4_blendInEnd"));
    }
}

TEST_CASE ("Event inheritance parser retains native selectors and defaults without lifetime channels", "[particle][event][parser]") {
    Project project {};
    JSON initializers = JSON::array (), operators = JSON::array ();
    static constexpr const char* modes[] = {"setcolor", "multiplycolor", "setopacity", "multiplyopacity",
        "setcoloropacity", "multiplycoloropacity", "setvelocity", "addvelocity", "setsize", "multiplysize",
        "setrotation", "addrotation", "setangularvelocity", "addangularvelocity", "maxlifetime"};
    for (const auto* mode : modes) {
        initializers.push_back ({{"name", "inheritinitialvaluefromevent"}, {"input", mode}});
        operators.push_back ({{"name", "inheritvaluefromevent"}, {"input", mode}});
    }
    initializers.push_back ({{"name", "inheritinitialvaluefromevent"}});
    operators.push_back ({{"name", "inheritvaluefromevent"}, {"blendoutstart", .75f}});
    const auto object = ObjectParser::parse (JSON {{"id",1}, {"particle", {
        {"initializer", initializers}, {"operator", operators}}}}, project);
    const auto* p = object->as<Particle> ();
    REQUIRE (p->initializers.size () == 16);
    REQUIRE (p->operators.size () == 16);
    for (uint32_t i = 0; i < 15; ++i) {
        REQUIRE (p->initializers[i]->as<InheritInitialValueFromEventInitializer> ()->mode == i);
        REQUIRE (p->operators[i]->as<InheritValueFromEventOperator> ()->mode == i);
    }
    REQUIRE (p->initializers[15]->as<InheritInitialValueFromEventInitializer> ()->mode == 0);
    const auto* op = p->operators[15]->as<InheritValueFromEventOperator> ();
    REQUIRE (op->mode == 4);
    REQUIRE (op->blendEnvelope->outStart->value->getFloat () == .75f);
}

TEST_CASE ("Control point birth velocity uses previous outer scene duration across scaled node passes", "[particle][controlpoint][clock]") {
    using namespace WallpaperEngine::Render::Objects::ParticleCore;
    WallpaperEngine::Render::Wallpapers::ParticleSceneFrameDurations scene;
    REQUIRE (scene.previous == 1.0f / 30.0f);
    scene.publish (.020f, 10);
    REQUIRE (scene.previous == 0.0f);
    scene.publish (.016f, 11);
    REQUIRE (scene.previous == .020f);
    scene.publish (.500f, 11); // another viewport does not advance the outer clock
    REQUIRE (scene.previous == .020f);
    REQUIRE (scene.current == .016f);
    for (float rate : {.5f, 1.f, 3.f}) {
        const auto node = tickClock (.016f, .016f * rate, 20);
        REQUIRE (node.operatorPasses == 2);
        std::mt19937 rng (7);
        const auto velocity = inheritedControlPointVelocity (rng, {2,0,0}, {0,0,0},
            scene.previous, 1.f, 1.f);
        REQUIRE (velocity.x == Catch::Approx (100.f));
        dispatchTick (node, [] (float) {}, [] {}, [&] (MovementTime) {
            REQUIRE (scene.previous == .020f);
        });
    }
    scene.publish (.030f, 12);
    REQUIRE (scene.previous == .016f);
}

TEST_CASE ("Birth angularspeed remap writes only the native angular stream", "[particle][birth][remap][flags]") {
    using namespace WallpaperEngine::Render::Objects::ParticleCore;
    ScalarRemapRange range;
    range.inputMin = 0; range.inputMax = 1; range.outputMin = 2; range.outputMax = 4;
    REQUIRE (remapBirthAngularSpeed (8, .5f, RemapOperation::Set, range, false, false) == 8);
    for (const char* allocation : {"rotationrandom", "angularvelocityrandom", "angularmovement"}) {
        Project project {};
        JSON initializers = JSON::array ({{{"name", "remapinitialvalue"}, {"output", "angularspeed"}}});
        JSON operators = JSON::array ();
        if (std::string (allocation) == "angularmovement") operators.push_back ({{"name", allocation}});
        else initializers.push_back ({{"name", allocation}});
        const auto object = ObjectParser::parse (JSON {{"id",1}, {"particle", {
            {"initializer", initializers}, {"operator", operators}}}}, project);
        const auto* p = object->as<Particle> ();
        const bool angularRandom = p->initializers.size () == 2
            && p->initializers[1]->is<AngularVelocityRandomInitializer> ();
        const bool movement = !p->operators.empty () && p->operators[0]->is<AngularMovementOperator> ();
        REQUIRE (remapBirthAngularSpeed (8, .5f, RemapOperation::Set, range, angularRandom, movement)
            == (std::string (allocation) == "rotationrandom" ? 8 : 3));
    }
}
