#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Render/Wallpapers/SceneDependencies.h"
#include "WallpaperEngine/Render/Wallpapers/SceneTransform.h"
#include "WallpaperEngine/Render/Objects/ModelNormalMatrix.h"

#include <cmath>
#include <string>

TEST_CASE ("Static model object retains path and authored skin", "[scene][parser][model]") {
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::SceneModel;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    auto parsed = ObjectParser::parse (JSON::parse (
        R"({"id":37,"name":"model","model":"models/neonsun/neonsun.mdl","skin":2,"origin":"1 2 3"})"), project);
    REQUIRE (parsed->is<SceneModel> ());
    REQUIRE (parsed->as<SceneModel> ()->path == "models/neonsun/neonsun.mdl");
    REQUIRE (parsed->as<SceneModel> ()->skin == 2);
    REQUIRE_THROWS_AS (ObjectParser::parse (JSON::parse (
        R"({"id":38,"name":"invalid","model":"models/a.mdl","skin":-1})"), project), std::invalid_argument);
}

TEST_CASE ("Point light retains shader-array properties without becoming a model", "[scene][parser][light]") {
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::ScenePointLight;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    auto parsed = ObjectParser::parse (JSON::parse (
        R"({"id":2,"name":"key","light":"point","model":null,"particle":null,"origin":"1 2 3","color":"0.25 0.5 1","intensity":1.75,"radius":9.5,"exponent":3})"), project);
    REQUIRE (parsed->is<ScenePointLight> ());
    const auto& light = *parsed->as<ScenePointLight> ();
    REQUIRE_FALSE (light.lightingV1);
    REQUIRE (light.origin->value->getVec3 () == glm::vec3 (1.0f, 2.0f, 3.0f));
    REQUIRE (light.color->value->getVec3 () == glm::vec3 (0.25f, 0.5f, 1.0f));
    REQUIRE (light.intensity->value->getFloat () == 1.75f);
    REQUIRE (light.radius->value->getFloat () == 9.5f);
    REQUIRE (light.exponent->value->getFloat () == 3.0f);
    auto defaults = ObjectParser::parse (JSON::parse (
        R"({"id":3,"name":"unset","light":"point"})"), project);
    REQUIRE (defaults->is<ScenePointLight> ());
    const auto& unset = *defaults->as<ScenePointLight> ();
    REQUIRE (unset.color->value->getVec3 () == glm::vec3 (0.0f));
    REQUIRE (unset.intensity->value->getFloat () == 0.0f);
    REQUIRE (unset.radius->value->getFloat () == 1.0f);
    REQUIRE (unset.exponent->value->getFloat () == 2.0f);
    auto modern = ObjectParser::parse (JSON::parse (
        R"({"id":4,"name":"modern","light":"lpoint","model":null})"), project);
    REQUIRE (modern->is<ScenePointLight> ());
    REQUIRE (modern->as<ScenePointLight> ()->lightingV1);
}

TEST_CASE ("Static model and generic parent retain X/Y rotations", "[scene][model]") {
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Render::Wallpapers::resolveSceneTransform;
    Project project {};
    auto parent = ObjectParser::parse (JSON::parse (
        R"({"id":1,"name":"parent","solid":true,"angles":"0 1.57079632679 0"})"), project);
    auto child = ObjectParser::parse (JSON::parse (
        R"({"id":2,"name":"model","model":"models/test.mdl","parent":1,"angles":"1.57079632679 0 0"})"), project);
    const auto result = resolveSceneTransform (*child,
        [&parent] (int id) -> const WallpaperEngine::Data::Model::Object* {
            return id == 1 ? parent.get () : nullptr;
        });
    const glm::vec4 rotated = result.authoredMatrix * glm::vec4 (0.0f, 1.0f, 0.0f, 1.0f);
    REQUIRE (std::abs (rotated.x - 1.0f) < 1e-5f);
    REQUIRE (std::abs (rotated.y) < 1e-5f);
    REQUIRE (std::abs (rotated.z) < 1e-5f);
}

TEST_CASE ("Native model normal matrix normalizes sheared basis columns", "[scene][model]") {
    glm::mat4 world (1.0f);
    world[0] = {2.0f, 0.0f, 0.0f, 0.0f};
    world[1] = {1.0f, 1.0f, 0.0f, 0.0f};
    world[2] = {0.0f, 0.0f, -4.0f, 0.0f};
    const auto normal = WallpaperEngine::Render::Objects::modelNormalMatrix (world);
    REQUIRE (normal.has_value ());
    REQUIRE (std::abs ((*normal)[0].x - 1.0f) < 1e-6f);
    REQUIRE (std::abs ((*normal)[1].x - std::sqrt (0.5f)) < 1e-6f);
    REQUIRE (std::abs ((*normal)[1].y - std::sqrt (0.5f)) < 1e-6f);
    REQUIRE (std::abs ((*normal)[2].z + 1.0f) < 1e-6f);
    const glm::mat3 inverseTranspose = glm::transpose (glm::inverse (glm::mat3 (world)));
    REQUIRE (std::abs ((*normal)[1].x - inverseTranspose[1].x) > 0.5f);
    world[2] = {0.0f, 0.0f, 0.0f, 0.0f};
    const auto planar = WallpaperEngine::Render::Objects::modelNormalMatrix (world);
    REQUIRE (planar.has_value ());
    REQUIRE ((*planar)[2] == glm::vec3 (0.0f));
}

TEST_CASE ("Parsed scene object graph rejects dependency and parent cycles", "[scene][parser]") {
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::ObjectList;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Render::Wallpapers::validateSceneDependencies;
    Project project {};
    auto parse = [&] (const char* json) { return ObjectParser::parse (JSON::parse (json), project); };

    ObjectList acyclic;
    acyclic.push_back (parse (R"({"id":1,"name":"root","solid":true})"));
    acyclic.push_back (parse (R"({"id":2,"name":"child","solid":true,"parent":1})"));
    acyclic.push_back (parse (R"({"id":3,"name":"dependent","solid":true,"dependencies":[2]})"));
    REQUIRE_NOTHROW (validateSceneDependencies (acyclic));

    ObjectList selfDependency;
    selfDependency.push_back (parse (R"({"id":1,"name":"self","solid":true,"dependencies":[1]})"));
    REQUIRE_NOTHROW (validateSceneDependencies (selfDependency));

    ObjectList cycle;
    cycle.push_back (parse (R"({"id":1,"name":"first","solid":true,"dependencies":[2]})"));
    cycle.push_back (parse (R"({"id":2,"name":"second","solid":true,"parent":1})"));
    try {
        validateSceneDependencies (cycle);
        FAIL ("Expected graph cycle");
    } catch (const std::invalid_argument& e) {
        REQUIRE (std::string (e.what ()).find ("1 -> 2 -> 1") != std::string::npos);
    }

    ObjectList selfParent;
    selfParent.push_back (parse (R"({"id":4,"name":"self-parent","solid":true,"parent":4})"));
    REQUIRE_THROWS_AS (validateSceneDependencies (selfParent), std::invalid_argument);

    ObjectList missingParent;
    missingParent.push_back (parse (R"({"id":5,"name":"orphan","solid":true,"parent":6})"));
    REQUIRE_THROWS_AS (validateSceneDependencies (missingParent), std::invalid_argument);

    ObjectList duplicate;
    duplicate.push_back (parse (R"({"id":7,"name":"a","solid":true})"));
    duplicate.push_back (parse (R"({"id":7,"name":"b","solid":true})"));
    REQUIRE_THROWS_AS (validateSceneDependencies (duplicate), std::invalid_argument);
}

TEST_CASE ("Typed scene dependencies retain target, slot and kind for later binding", "[scene][parser]") {
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::ObjectList;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Render::Wallpapers::validateSceneDependencies;
    Project project {};
    ObjectList objects;
    objects.push_back (ObjectParser::parse (JSON::parse (
        R"({"id":38,"name":"image","solid":true})"), project));
    objects.push_back (ObjectParser::parse (JSON::parse (
        R"({"id":19,"name":"particles","solid":true,
             "dependencies":[{"id":38,"index":0,"type":"emitterimage",
                              "mask":"textures/emission-mask.png"}]})"), project));
    REQUIRE (objects[1]->dependencies.size () == 1);
    REQUIRE (objects[1]->dependencies[0] == 38);
    REQUIRE (objects[1]->typedDependencies.size () == 1);
    REQUIRE (objects[1]->typedDependencies[0].id == 38);
    REQUIRE (objects[1]->typedDependencies[0].index == 0);
    REQUIRE (objects[1]->typedDependencies[0].type == "emitterimage");
    REQUIRE (objects[1]->typedDependencies[0].mask == "textures/emission-mask.png");
    REQUIRE_NOTHROW (validateSceneDependencies (objects));
}

TEST_CASE ("Invalid dependency branches leave unrelated scene objects available", "[scene][parser]") {
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::ObjectList;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Render::Wallpapers::inspectSceneDependencies;
    Project project {};
    auto parse = [&] (const char* json) { return ObjectParser::parse (JSON::parse (json), project); };
    ObjectList objects;
    objects.push_back (parse (R"({"id":1,"name":"independent-a","solid":true})"));
    objects.push_back (parse (R"({"id":2,"name":"orphan","solid":true,"parent":99})"));
    objects.push_back (parse (R"({"id":3,"name":"orphan-child","solid":true,"parent":2})"));
    objects.push_back (parse (R"({"id":4,"name":"child-dependent","solid":true,"dependencies":[3]})"));
    objects.push_back (parse (R"({"id":5,"name":"independent-b","solid":true})"));
    objects.push_back (parse (R"({"id":6,"name":"cycle-a","solid":true,"dependencies":[7]})"));
    objects.push_back (parse (R"({"id":7,"name":"cycle-b","solid":true,"dependencies":[6]})"));
    objects.push_back (parse (R"({"id":8,"name":"cycle-dependent","solid":true,"parent":6})"));
    objects.push_back (parse (R"({"id":9,"name":"missing-dependency","solid":true,"dependencies":[999]})"));
    const auto report = inspectSceneDependencies (objects);
    REQUIRE_FALSE (report.rejected.contains (1));
    REQUIRE_FALSE (report.rejected.contains (5));
    for (int id : {2, 3, 4, 6, 7, 8, 9}) REQUIRE (report.rejected.contains (id));
    REQUIRE (report.rejected.at (3).find ("invalid object 2") != std::string::npos);
    REQUIRE (report.rejected.at (6).find ("cycle") != std::string::npos);
}
