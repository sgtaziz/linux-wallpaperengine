#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/WallpaperParser.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Render/Wallpapers/SceneDependencies.h"
#include "WallpaperEngine/Render/Camera.h"
#include "WallpaperEngine/Render/Wallpapers/SceneCursor.h"
#include "WallpaperEngine/Render/Wallpapers/SceneTransform.h"
#include "WallpaperEngine/Input/MouseInput.h"
#include "WallpaperEngine/Render/Shaders/ShaderUnit.h"
#include "WallpaperEngine/Scripting/SceneCameraTransforms.h"

#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <memory>
#include <string>
#include <tuple>

TEST_CASE ("Native layer input defaults survive authored configuration cloning",
           "[scene][parser][script][input]") {
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    // Native omitted flags are solid=true and disablepropagation=false. The
    // detached config must preserve omission instead of serializing defaults.
    auto omitted = ObjectParser::parse (JSON::parse (R"({"id":1,"name":"group"})"), project);
    REQUIRE (omitted->solid);
    REQUIRE_FALSE (omitted->disablePropagation);
    auto config = JSON::parse (omitted->initialConfiguration);
    REQUIRE_FALSE (config.contains ("solid"));
    REQUIRE_FALSE (config.contains ("disablepropagation"));
    config["id"] = 2;
    auto clone = ObjectParser::parse (config, project);
    REQUIRE (clone->solid);
    REQUIRE_FALSE (clone->disablePropagation);

    auto explicitFlags = ObjectParser::parse (JSON::parse (
        R"({"id":3,"name":"group","solid":false,"disablepropagation":true})"), project);
    REQUIRE_FALSE (explicitFlags->solid);
    REQUIRE (explicitFlags->disablePropagation);
    config = JSON::parse (explicitFlags->initialConfiguration);
    REQUIRE (config.at ("solid") == false);
    REQUIRE (config.at ("disablepropagation") == true);
    config["id"] = 4;
    clone = ObjectParser::parse (config, project);
    REQUIRE_FALSE (clone->solid);
    REQUIRE (clone->disablePropagation);
    config["solid"] = true;
    REQUIRE_FALSE (JSON::parse (explicitFlags->initialConfiguration).at ("solid").get<bool> ());
    REQUIRE_FALSE (clone->solid);
}

TEST_CASE ("Scene camera projection settings prefer authored general fields", "[scene][parser]") {
    using WallpaperEngine::Assets::AssetLocator;
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Scene;
    using WallpaperEngine::Data::Parsers::WallpaperParser;
    using WallpaperEngine::FileSystem::Container;

    auto files = std::make_unique<Container> ();
    files->getVFS ().add ("scene.json", R"({
      "camera":{"center":"0 0 0","eye":"0 0 1","up":"0 1 0",
                "nearz":1,"farz":100,"fov":30},
      "general":{"orthogonalprojection":{"width":100,"height":100},
                 "nearz":0.25,"farz":250,"fov":75,"zoom":1.25,},
      "objects":[],
    })");
    Project project {};
    project.type = Project::Type_Scene;
    project.assetLocator = std::make_unique<AssetLocator> (std::move (files));
    const auto wallpaper = WallpaperParser::parse (JSON ("scene.json"), project);
    const auto* scene = dynamic_cast<const Scene*> (wallpaper.get ());
    REQUIRE (scene != nullptr);
    REQUIRE (scene->camera.projection.nearz->value->getFloat () == 0.25f);
    REQUIRE (scene->camera.projection.farz->value->getFloat () == 250.0f);
    REQUIRE (scene->camera.projection.fov->value->getFloat () == 75.0f);
    REQUIRE (scene->camera.projection.zoom->value->getFloat () == 1.25f);
    REQUIRE (scene->camera.projection.perspectiveOverrideFov->value->getFloat () == 95.0f);
}

TEST_CASE ("Orthographic perspective override FOV is independent of perspective FOV",
           "[scene][parser][particle][projection]") {
    using WallpaperEngine::Assets::AssetLocator;
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Scene;
    using WallpaperEngine::Data::Parsers::WallpaperParser;
    using WallpaperEngine::FileSystem::Container;
    auto files = std::make_unique<Container> ();
    files->getVFS ().add ("scene.json", R"({
      "camera":{"center":"0 0 -1","eye":"0 0 0","up":"0 1 0"},
      "general":{"orthogonalprojection":{"width":3840,"height":2160},
                 "fov":50,"perspectiveoverridefov":67},
      "objects":[]})");
    Project project {};
    project.type = Project::Type_Scene;
    project.assetLocator = std::make_unique<AssetLocator> (std::move (files));
    const auto wallpaper = WallpaperParser::parse (JSON ("scene.json"), project);
    const auto* scene = dynamic_cast<const Scene*> (wallpaper.get ());
    REQUIRE (scene != nullptr);
    REQUIRE (scene->camera.projection.fov->value->getFloat () == 50.0f);
    REQUIRE (scene->camera.projection.perspectiveOverrideFov->value->getFloat () == 67.0f);
}

TEST_CASE ("Scene camera projection settings retain camera-field fallback", "[scene][parser]") {
    using WallpaperEngine::Assets::AssetLocator;
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Scene;
    using WallpaperEngine::Data::Parsers::WallpaperParser;
    using WallpaperEngine::FileSystem::Container;
    auto files = std::make_unique<Container> ();
    files->getVFS ().add ("scene.json", R"({
      "camera":{"center":"0 0 0","eye":"0 0 1","up":"0 1 0",
                "nearz":1,"farz":100,"fov":30},
      "general":{"orthogonalprojection":{"width":100,"height":100}},
      "objects":[]
    })");
    Project project {};
    project.type = Project::Type_Scene;
    project.assetLocator = std::make_unique<AssetLocator> (std::move (files));
    const auto wallpaper = WallpaperParser::parse (JSON ("scene.json"), project);
    const auto* scene = dynamic_cast<const Scene*> (wallpaper.get ());
    REQUIRE (scene != nullptr);
    REQUIRE (scene->camera.projection.nearz->value->getFloat () == 1.0f);
    REQUIRE (scene->camera.projection.farz->value->getFloat () == 100.0f);
    REQUIRE (scene->camera.projection.fov->value->getFloat () == 30.0f);
}

TEST_CASE ("Scene without orthogonalprojection selects the native perspective branch", "[scene][parser]") {
    using WallpaperEngine::Assets::AssetLocator;
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Scene;
    using WallpaperEngine::Data::Parsers::WallpaperParser;
    using WallpaperEngine::FileSystem::Container;
    auto files = std::make_unique<Container> ();
    files->getVFS ().add ("scene.json", R"({
      "camera":{"center":"0 0 0","eye":"0 0 1000","up":"0 1 0"},
      "general":{},"objects":[]})");
    Project project {};
    project.type = Project::Type_Scene;
    project.assetLocator = std::make_unique<AssetLocator> (std::move (files));
    const auto wallpaper = WallpaperParser::parse (JSON ("scene.json"), project);
    const auto* scene = dynamic_cast<const Scene*> (wallpaper.get ());
    REQUIRE (scene != nullptr);
    REQUIRE_FALSE (scene->camera.projection.isOrthogonal);
    REQUIRE_FALSE (scene->camera.projection.isAuto);
    REQUIRE (scene->camera.projection.nearz->value->getFloat () == 0.1f);
    REQUIRE (scene->camera.projection.farz->value->getFloat () == 10000.0f);
    REQUIRE (scene->camera.projection.fov->value->getFloat () == 50.0f);
}

TEST_CASE ("Last active camera object supplies the native scene pose and FOV", "[scene][camera]") {
    using WallpaperEngine::Assets::AssetLocator;
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Scene;
    using WallpaperEngine::Data::Parsers::WallpaperParser;
    using WallpaperEngine::FileSystem::Container;
    using WallpaperEngine::Render::Camera;

    auto files = std::make_unique<Container> ();
    files->getVFS ().add ("scene.json", R"({
      "camera":{"center":"2 2 1","eye":"2 2 2","up":"0 1 0"},
      "general":{"fov":125},
      "objects":[
        {"id":1,"name":"first","camera":"default","origin":"0 0 5","fov":60},
        {"id":2,"name":"native-281","camera":"default",
         "origin":"-0.30007 -0.04811 3.77126","angles":"0.36399 -0.49698 0","fov":45},
        {"id":3,"name":"hidden","camera":"default","visible":false,"fov":15}
      ]})");
    Project project {};
    project.type = Project::Type_Scene;
    project.assetLocator = std::make_unique<AssetLocator> (std::move (files));
    const auto wallpaper = WallpaperParser::parse (JSON ("scene.json"), project);
    const auto* scene = dynamic_cast<const Scene*> (wallpaper.get ());
    REQUIRE (scene != nullptr);
    const auto* selected = Camera::selectActiveSceneCamera (scene->objects);
    REQUIRE (selected != nullptr);
    REQUIRE (selected->id == 2);
    REQUIRE (selected->fov->value->getFloat () == 45.0f);
    REQUIRE (scene->camera.projection.fov->value->getFloat () == 125.0f);
    const auto pose = Camera::poseForSceneCamera (*selected);
    const auto near = [] (float actual, float expected) { return std::abs (actual - expected) < 2e-4f; };
    REQUIRE (near (pose.eye.x, -0.30007f));
    REQUIRE (near (pose.eye.y, -0.04811f));
    REQUIRE (near (pose.eye.z, 3.77126f));
    REQUIRE (near (pose.center.x, 0.14547f));
    REQUIRE (near (pose.center.y, 0.30790f));
    REQUIRE (near (pose.center.z, 2.94982f));
    REQUIRE (near (pose.up.x, -0.16973f));
    REQUIRE (near (pose.up.y, 0.93448f));
    REQUIRE (near (pose.up.z, 0.31294f));
    const auto objectProjection = Camera::makePerspectiveProjectionForScene (1280, 720, 45, 0.1f, 1000);
    const auto rootProjection = Camera::makePerspectiveProjectionForScene (1280, 720, 125, 0.1f, 1000);
    REQUIRE (objectProjection[0][0] > rootProjection[0][0]);
}

TEST_CASE ("Signed nonzero orthographic dimensions select the native context mode",
           "[scene][parser][particle]") {
    using WallpaperEngine::Assets::AssetLocator;
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Scene;
    using WallpaperEngine::Data::Parsers::WallpaperParser;
    using WallpaperEngine::FileSystem::Container;
    for (const auto [width, height, expected] : {
             std::tuple {-100, 100, true}, std::tuple {100, -100, true},
             std::tuple {-100, -100, true}, std::tuple {0, 100, false}}) {
        auto files = std::make_unique<Container> ();
        files->getVFS ().add ("scene.json", "{\"camera\":{\"center\":\"0 0 0\",\"eye\":\"0 0 1\",\"up\":\"0 1 0\"},"
            "\"general\":{\"orthogonalprojection\":{\"width\":" + std::to_string (width)
            + ",\"height\":" + std::to_string (height) + "}},\"objects\":[]}");
        Project project {};
        project.type = Project::Type_Scene;
        project.assetLocator = std::make_unique<AssetLocator> (std::move (files));
        const auto wallpaper = WallpaperParser::parse (JSON ("scene.json"), project);
        const auto* scene = dynamic_cast<const Scene*> (wallpaper.get ());
        REQUIRE (scene != nullptr);
        REQUIRE (scene->camera.projection.isOrthogonal == expected);
    }
}

TEST_CASE ("Perspective projection responds to FOV, aspect and clip planes", "[scene][camera]") {
    using WallpaperEngine::Render::Camera;
    const auto baseline = Camera::makePerspectiveProjectionForScene (200, 100, 60, 1, 1000);
    const auto narrow = Camera::makePerspectiveProjectionForScene (200, 100, 30, 1, 1000);
    REQUIRE (narrow[0][0] > baseline[0][0]);
    // The OpenGL final scene blit reverses V relative to native presentation;
    // its perspective projection compensates once without changing FOV scale.
    REQUIRE (baseline[1][1] < 0.0f);
    REQUIRE (std::abs (narrow[1][1]) > std::abs (baseline[1][1]));
    const auto widerAspect = Camera::makePerspectiveProjectionForScene (400, 100, 60, 1, 1000);
    REQUIRE (widerAspect[0][0] < baseline[0][0]);
    REQUIRE (widerAspect[1][1] == baseline[1][1]);
    for (float z : {-1.0f, -1000.0f}) {
        const glm::vec4 clip = baseline * glm::vec4 (0, 0, z, 1);
        REQUIRE (clip.w > 0.0f);
        REQUIRE (std::abs (std::abs (clip.z / clip.w) - 1.0f) < 1e-4f);
    }
    REQUIRE_THROWS (Camera::makePerspectiveProjectionForScene (200, 100, 60, 0, 1000));
    REQUIRE_THROWS (Camera::makePerspectiveProjectionForScene (200, 100, 60, 10, 1));
    REQUIRE_THROWS (Camera::makePerspectiveProjectionForScene (200, 100, 180, 1, 1000));
}

TEST_CASE ("Perspective scene points retain authored world coordinates across viewports", "[scene][camera]") {
    using WallpaperEngine::Render::Wallpapers::scenePointForCamera;
    const glm::vec3 authored {25.0f, -10.0f, 30.0f};
    REQUIRE (scenePointForCamera (authored, 512, 256, false) == authored);
    REQUIRE (scenePointForCamera (authored, 1024, 512, false) == authored);
    REQUIRE (scenePointForCamera (authored, 512, 256, true) == glm::vec3 (-231, 138, 30));
    REQUIRE (scenePointForCamera (authored, 1024, 512, true) == glm::vec3 (-487, 266, 30));
}

TEST_CASE ("Orthographic scene projection keeps the z-zero image plane visible", "[scene][camera]") {
    using WallpaperEngine::Render::Camera;
    const glm::vec4 imagePlane { 0.0f, 0.0f, 0.0f, 1.0f };
    const glm::vec3 eye { 0.0f, 0.0f, 0.0f };
    const auto baseline = Camera::makeOrthogonalProjectionForScene (1920.0f, 1080.0f, 0.0f, 10000.0f, eye);

    for (float authoredNear : { 0.0f, 0.01f, 0.1f }) {
        const auto projection = Camera::makeOrthogonalProjectionForScene (
            1920.0f, 1080.0f, authoredNear, 10000.0f, eye);
        const glm::vec4 clip = projection * imagePlane;
        REQUIRE (clip.w > 0.0f);
        REQUIRE (clip.z >= -clip.w);
        REQUIRE (clip.z <= clip.w);
        REQUIRE (projection[2][2] == baseline[2][2]);
        REQUIRE (projection[3][2] == baseline[3][2]);
    }

    // CImage uses projection * lookAt. Test the actual composed path with
    // a translated eye, including positive z. Native orthographic projection
    // ignores authored near/far while retaining the existing view convention.
    const glm::vec3 movedEye { 10.0f, -5.0f, 1.0f };
    const glm::mat4 movedView = glm::lookAt (
        movedEye, movedEye + glm::vec3 (0.0f, 0.0f, -1.0f), glm::vec3 (0.0f, 1.0f, 0.0f));
    const auto negativeNear = Camera::makeOrthogonalProjectionForScene (
        1920.0f, 1080.0f, -2.0f, 10000.0f, movedEye);
    const glm::vec4 movedClip = negativeNear * movedView * imagePlane;
    REQUIRE (movedClip.z >= -movedClip.w);
    REQUIRE (movedClip.z <= movedClip.w);
    REQUIRE (negativeNear[2][2] == baseline[2][2]);

    const auto positiveNear = Camera::makeOrthogonalProjectionForScene (
        1920.0f, 1080.0f, 0.1f, 10000.0f, movedEye);
    const glm::vec4 movedPositiveClip = positiveNear * movedView * imagePlane;
    REQUIRE (movedPositiveClip.z >= -movedPositiveClip.w);
    REQUIRE (movedPositiveClip.z <= movedPositiveClip.w);

    const auto invalid = Camera::makeOrthogonalProjectionForScene (
        1920.0f, 1080.0f, std::numeric_limits<float>::quiet_NaN (),
        std::numeric_limits<float>::infinity (), eye);
    const glm::vec4 invalidClip = invalid * imagePlane;
    REQUIRE (std::isfinite (invalidClip.z));
    REQUIRE (invalidClip.z >= -invalidClip.w);
    REQUIRE (invalidClip.z <= invalidClip.w);

    const auto negativeFar = Camera::makeOrthogonalProjectionForScene (
        1920.0f, 1080.0f, -100.0f, -1.0f, eye);
    const glm::vec4 negativeFarClip = negativeFar * imagePlane;
    REQUIRE (negativeFarClip.z >= -negativeFarClip.w);
    REQUIRE (negativeFarClip.z <= negativeFarClip.w);
}

TEST_CASE ("Orthographic native clip range preserves XYZ geometry on both sides of z-zero", "[scene][camera][transform]") {
    using WallpaperEngine::Render::Camera;
    // Native 140183a70 selects -2000/+2000 for scene bit8, independently
    // of authored perspective planes. Actual rotated text at z=0 spans
    // positive and negative Z; a near plane at zero clips its left glyphs.
    const auto projection = Camera::makeOrthogonalProjectionForScene (
        3840.0f, 2160.0f, 0.01f, 10000.0f, glm::vec3 (0));
    for (const float z : {-2000.0f, -1999.0f, -75.89421f, 0.0f, 75.89421f, 1999.0f, 2000.0f}) {
        const auto clip = projection * glm::vec4 (100, 200, z, 1);
        REQUIRE (clip.z == Catch::Approx (-z / 2000.0f));
        REQUIRE (clip.z >= -clip.w);
        REQUIRE (clip.z <= clip.w);
        REQUIRE (clip.x == Catch::Approx (100.0f / 1920.0f));
        REQUIRE (clip.y == Catch::Approx (200.0f / 1080.0f));
    }
    for (const float z : {-2001.0f, 2001.0f}) {
        const auto clip = projection * glm::vec4 (0, 0, z, 1);
        REQUIRE (std::abs (clip.z) > clip.w);
    }
    REQUIRE (Camera::makeOrthogonalProjectionForScene (
        3840.0f, 2160.0f, -2000.0f, 2000.0f, glm::vec3 (0)) == projection);
    REQUIRE (Camera::makeOrthogonalProjectionForScene (
        3840.0f, 2160.0f, 1.0f, 2.0f, glm::vec3 (0)) == projection);
    // Native 1401dd630's Z row for the actual Clock's X/Y angles and
    // scale. Its centered quad crosses z=0 even though the layer origin
    // lies exactly on that plane; retain all four projected corners.
    for (const float x : {-100.0f, 100.0f}) {
        for (const float y : {-50.0f, 50.0f}) {
            const float z = 1.50877f * (-std::sin (0.52709f) * x
                + std::sin (-0.21384f) * std::cos (0.52709f) * y);
            const auto clip = projection * glm::vec4 (x, y, z, 1);
            REQUIRE (clip.z >= -clip.w);
            REQUIRE (clip.z <= clip.w);
        }
    }
}

TEST_CASE ("Scene cursor converts bottom-left viewport pixels through cropped UV and camera", "[scene][input]") {
    using WallpaperEngine::Render::Camera;
    using WallpaperEngine::Render::Wallpapers::cursorScreenPosition;
    using WallpaperEngine::Render::Wallpapers::cursorWorldPosition;
    using WallpaperEngine::Render::Wallpapers::cursorHitLocalPosition;
    using WallpaperEngine::Render::Wallpapers::cursorLocalPosition;
    const auto screen = cursorScreenPosition ({175.0, 90.0}, {100, 50, 200, 100});
    REQUIRE (screen.x == 75.0f);
    REQUIRE (screen.y == 60.0f);
    const auto framebuffer = WallpaperEngine::Input::cursorFramebufferPosition (
        {50.0, 25.0}, {200, 100}, {400, 300});
    REQUIRE (framebuffer.x == 100.0);
    REQUIRE (framebuffer.y == 225.0);
    const auto projection = Camera::makeOrthogonalProjectionForScene (
        100.0f, 80.0f, 0.0f, 1000.0f, {0.0f, 0.0f, 0.0f});
    const auto center = cursorWorldPosition ({0.5f, 0.5f}, projection, 100.0f, 80.0f);
    REQUIRE (center.has_value ());
    REQUIRE (std::abs (center->x - 50.0f) < 1e-4f);
    REQUIRE (std::abs (center->y - 40.0f) < 1e-4f);
    const auto crop = cursorWorldPosition ({0.25f, 0.75f}, projection, 100.0f, 80.0f);
    REQUIRE (crop.has_value ());
    REQUIRE (std::abs (crop->x - 25.0f) < 1e-4f);
    REQUIRE (std::abs (crop->y - 60.0f) < 1e-4f);
    const auto shifted = Camera::makeOrthogonalProjectionForScene (
        100.0f, 80.0f, 0.0f, 1000.0f, {10.0f, -5.0f, 0.0f});
    const auto moved = cursorWorldPosition ({0.25f, 0.75f}, shifted, 100.0f, 80.0f);
    REQUIRE (moved.has_value ());
    REQUIRE (std::abs (moved->x - 15.0f) < 1e-4f);
    REQUIRE (std::abs (moved->y - 65.0f) < 1e-4f);
    const glm::mat4 layer = glm::translate (glm::mat4 (1.0f), {50.0f, 40.0f, 0.0f})
        * glm::rotate (glm::mat4 (1.0f), 0.5f, {0.0f, 0.0f, 1.0f});
    const glm::vec3 point = glm::vec3 (layer * glm::vec4 (8.0f, -3.0f, 0.0f, 1.0f));
    const auto local = cursorHitLocalPosition (point, layer, {40.0f, 20.0f});
    REQUIRE (local.has_value ());
    REQUIRE (std::abs (local->x - 28.0f) < 1e-4f);
    REQUIRE (std::abs (local->y - 13.0f) < 1e-4f);
    const glm::vec3 outside = glm::vec3 (layer * glm::vec4 (22.0f, 0.0f, 0.0f, 1.0f));
    REQUIRE_FALSE (cursorHitLocalPosition (outside, layer, {40.0f, 20.0f}).has_value ());
    const auto leave = cursorLocalPosition (outside, layer, {40.0f, 20.0f});
    REQUIRE (leave.has_value ());
    REQUIRE (std::abs (leave->x - 42.0f) < 1e-4f);
    const glm::vec2 alignment {20.0f, -10.0f};
    const glm::vec3 alignedCenter = glm::vec3 (layer * glm::vec4 (alignment, 0.0f, 1.0f));
    const auto aligned = cursorHitLocalPosition (alignedCenter, layer, {40.0f, 20.0f}, alignment);
    REQUIRE (aligned.has_value ());
    REQUIRE (std::abs (aligned->x - 20.0f) < 1e-4f);
    REQUIRE (std::abs (aligned->y - 10.0f) < 1e-4f);
    const auto large = Camera::makeOrthogonalProjectionForScene (
        100000.0f, 80000.0f, 0.0f, 100000.0f, {0.0f, 0.0f, 0.0f});
    const auto largeCenter = cursorWorldPosition ({0.5f, 0.5f}, large, 100000.0f, 80000.0f);
    REQUIRE (largeCenter.has_value ());
    REQUIRE (std::abs (largeCenter->x - 50000.0f) < 0.1f);
    REQUIRE (std::abs (largeCenter->y - 40000.0f) < 0.1f);
}

TEST_CASE ("Scene cursor world coordinates retain the native authored canvas across viewport and crop",
           "[scene][input][solid]") {
    using WallpaperEngine::Render::Camera;
    using WallpaperEngine::Render::Wallpapers::cursorScreenPosition;
    using WallpaperEngine::Render::Wallpapers::cursorWorldPosition;
    const auto projection = Camera::makeOrthogonalProjectionForScene (
        1024.0f, 576.0f, 0.0f, 1000.0f, {0.0f, 0.0f, 0.0f});
    // Independent native real-pointer readouts on the 1280x720 viewport.
    for (const glm::vec4 landmark : {glm::vec4 (1186, 70, 948.8f, 520),
                                    glm::vec4 (501, 182, 400.8f, 430.4f)}) {
        const glm::vec2 uv (landmark.x / 1280, 1 - landmark.y / 720);
        const auto world = cursorWorldPosition (uv, projection, 1024, 576);
        REQUIRE (world.has_value ());
        REQUIRE (std::abs (world->x - landmark.z) < 1e-3f);
        REQUIRE (std::abs (world->y - landmark.w) < 1e-3f);
    }
    for (const glm::vec2 uv : {glm::vec2 (0, 0), glm::vec2 (1, 0), glm::vec2 (0, 1),
                              glm::vec2 (1, 1), glm::vec2 (.5f, .5f)}) {
        const auto world = cursorWorldPosition (uv, projection, 1024, 576);
        REQUIRE (world.has_value ());
        REQUIRE (std::abs (world->x - uv.x * 1024) < 1e-4f);
        REQUIRE (std::abs (world->y - uv.y * 576) < 1e-4f);
    }
    // Native positive non-square real-pointer gate: screen1187.5,70 maps
    // to authored950,520. Offset viewport changes screen origin, not world.
    const glm::ivec4 viewport (100, 50, 1280, 720);
    const glm::dvec2 pixel (1287.5, 700);
    const auto screen = cursorScreenPosition (pixel, viewport);
    REQUIRE (screen.x == 1187.5f);
    REQUIRE (screen.y == 70.0f);
    const glm::vec2 uv ((pixel.x - viewport.x) / viewport.z,
                       (pixel.y - viewport.y) / viewport.w);
    const auto world = cursorWorldPosition (uv, projection, 1024, 576);
    REQUIRE (world.has_value ());
    REQUIRE (std::abs (world->x - 950.0f) < 1e-4f);
    REQUIRE (std::abs (world->y - 520.0f) < 1e-4f);
    // Captured native asymmetric event payloads: the authored canvas is
    // bottom-left, while each layer's localPosition has a top-left origin.
    const glm::mat4 top = glm::translate (glm::mat4 (1.0f), {430.0f, 430.0f, 0.0f});
    const auto upper = WallpaperEngine::Render::Wallpapers::cursorHitLocalPosition (
        {400.0f, 480.0f, 0.0f}, top, {300.0f, 180.0f});
    REQUIRE (upper.has_value ());
    REQUIRE (std::abs (upper->x - 120.0f) < 1e-4f);
    REQUIRE (std::abs (upper->y - 40.0f) < 1e-4f);
    const glm::mat4 bottom = glm::translate (glm::mat4 (1.0f), {330.0f, 430.0f, 0.0f});
    const auto lower = WallpaperEngine::Render::Wallpapers::cursorHitLocalPosition (
        {400.0f, 380.8f, 0.0f}, bottom, {300.0f, 180.0f});
    REQUIRE (lower.has_value ());
    REQUIRE (std::abs (lower->x - 220.0f) < 1e-4f);
    REQUIRE (std::abs (lower->y - 139.2f) < 1e-3f);
    // A cropped/reversed presentation supplies its actual UV window. Keep
    // that mapping instead of another unconditional flip inside the query.
    const glm::vec4 window (.2f, .8f, .9f, .1f);
    const glm::vec2 cropped (window.x + .25f * (window.y - window.x),
                            window.z + .75f * (window.w - window.z));
    const auto crop = cursorWorldPosition (cropped, projection, 1024, 576);
    REQUIRE (crop.has_value ());
    REQUIRE (std::abs (crop->x - .35f * 1024) < 1e-4f);
    REQUIRE (std::abs (crop->y - .3f * 576) < 1e-4f);
}

TEST_CASE ("Malformed object parsing leaves unrelated scene layers intact", "[scene][parser]") {
    using WallpaperEngine::Assets::AssetLocator;
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Scene;
    using WallpaperEngine::Data::Parsers::WallpaperParser;
    using WallpaperEngine::FileSystem::Container;
    using WallpaperEngine::Render::Wallpapers::inspectSceneDependencies;
    auto files = std::make_unique<Container> ();
    files->getVFS ().add ("scene.json", R"({
      "camera":{"center":"0 0 0","eye":"0 0 1","up":"0 1 0"},
      "general":{"orthogonalprojection":{"width":100,"height":100}},
      "objects":[
        {"id":1,"name":"intact-a","solid":true},
        {"id":2,"name":"invalid-particle","particle":{"maxcount":-1}},
        {"id":3,"name":"child-of-invalid","solid":true,"parent":2},
        {"id":4,"name":"intact-b","solid":true}
      ]
    })");
    Project project {};
    project.type = Project::Type_Scene;
    project.assetLocator = std::make_unique<AssetLocator> (std::move (files));
    const auto wallpaper = WallpaperParser::parse (JSON ("scene.json"), project);
    const auto* scene = dynamic_cast<const Scene*> (wallpaper.get ());
    REQUIRE (scene != nullptr);
    REQUIRE (scene->objects.size () == 3);
    const auto report = inspectSceneDependencies (scene->objects);
    REQUIRE_FALSE (report.rejected.contains (1));
    REQUIRE (report.rejected.contains (3));
    REQUIRE_FALSE (report.rejected.contains (4));
}

TEST_CASE ("Scene light slots come from general rather than a root lookalike", "[scene][parser][light]") {
    using WallpaperEngine::Assets::AssetLocator;
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Scene;
    using WallpaperEngine::Data::Parsers::WallpaperParser;
    using WallpaperEngine::FileSystem::Container;
    const auto parse = [] (const JSON& general) {
        auto files = std::make_unique<Container> ();
        const JSON data {{"camera", {{"center", "0 0 -1"}, {"eye", "0 0 0"}, {"up", "0 1 0"}}},
                         {"general", general}, {"lightconfig", {{"point", 9}}}, {"objects", JSON::array ()}};
        files->getVFS ().add ("scene.json", data.dump ());
        Project project {};
        project.type = Project::Type_Scene;
        project.assetLocator = std::make_unique<AssetLocator> (std::move (files));
        const auto wallpaper = WallpaperParser::parse (JSON ("scene.json"), project);
        const auto* scene = dynamic_cast<const Scene*> (wallpaper.get ());
        REQUIRE (scene != nullptr);
        return scene->pointLightSlots;
    };
    const int slots = parse (JSON {{"lightconfig", {{"point", 2}, {"spot", 1}}}});
    CHECK (slots == 2);
    auto shaderFiles = std::make_unique<Container> ();
    AssetLocator shaderAssets (std::move (shaderFiles));
    using WallpaperEngine::Render::Shaders::ShaderUnit;
    using WallpaperEngine::Render::Shaders::GLSLContext;
    const WallpaperEngine::Data::Model::ShaderConstantMap constants;
    const WallpaperEngine::Data::Model::TextureMap textures;
    const WallpaperEngine::Data::Model::ComboMap combos, overrides {{"LIGHTS_POINT", slots}};
    ShaderUnit shader (GLSLContext::UnitType_Fragment, "point-slots.frag",
        "uniform vec4 g_LPoint_Color[LIGHTS_POINT];\n"
        "void main() { vec4 light = vec4(0);\n"
        "for (uint l = 0u; l < CASTU(LIGHTS_POINT); ++l) light += g_LPoint_Color[l];\n"
        "gl_FragColor = light; }\n", shaderAssets, constants, textures, textures, combos, overrides);
    // CPass passes the parsed serialized count as the LIGHTS_POINT override.
    REQUIRE (shader.compile ().find ("#define LIGHTS_POINT 2\n") != std::string::npos);
    ShaderUnit vertex (GLSLContext::UnitType_Vertex, "point-slots.vert",
        "attribute vec3 a_Position;\nvoid main() { gl_Position = vec4(a_Position, 1); }\n",
        shaderAssets, constants, textures, textures, combos, overrides);
    const auto translated = GLSLContext::get ().toGlsl (vertex.compile (), shader.compile ());
    REQUIRE_FALSE (translated.first.empty ());
    REQUIRE_FALSE (translated.second.empty ());
    CHECK (parse (JSON::object ()) == 0);
    CHECK (parse (JSON {{"lightconfig", {{"spot", 1}}}}) == 0);
    CHECK (parse (JSON {{"lightconfig", {{"point", 15}}}}) == 15);
}

TEST_CASE ("Orthographic root camera pose resets only without parsed paths", "[scene][camera][parallax]") {
    using WallpaperEngine::Render::Camera;
    WallpaperEngine::Data::Model::SceneData::Camera data {};
    data.configuration.eye = {-47.67039f, 204.18417f, 8};
    data.configuration.center = {17, -23, -1};
    data.configuration.up = {1, 0, 0};
    data.projection.isOrthogonal = true;
    const auto reset = Camera::poseForRootCamera (data);
    REQUIRE (reset.eye == glm::vec3 (0));
    REQUIRE (reset.center == glm::vec3 (0, 0, -1));
    REQUIRE (reset.up == glm::vec3 (0, 1, 0));
    data.configuration.hasPaths = true;
    REQUIRE (Camera::poseForRootCamera (data).eye == data.configuration.eye);
    data.configuration.hasPaths = false;
    data.projection.isOrthogonal = false;
    REQUIRE (Camera::poseForRootCamera (data).eye == data.configuration.eye);
}

TEST_CASE ("Root camera path presence follows parsed resource segments", "[scene][camera][parser]") {
    using WallpaperEngine::Assets::AssetLocator;
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Scene;
    using WallpaperEngine::Data::Parsers::WallpaperParser;
    using WallpaperEngine::FileSystem::Container;
    const auto parse = [] (const JSON& paths, const JSON& segments) {
        auto files = std::make_unique<Container> ();
        const JSON scene {{"camera", {{"center", "0 0 -1"}, {"eye", "17 -23 0"},
                                      {"up", "0 1 0"}, {"paths", paths}}},
                          {"general", {{"orthogonalprojection", {{"width", 512}, {"height", 256}}}}},
                          {"objects", JSON::array ()}};
        files->getVFS ().add ("scene.json", scene.dump ());
        files->getVFS ().add ("cameras/pose.json", JSON {{"paths", segments}}.dump ());
        Project project {};
        project.type = Project::Type_Scene;
        project.assetLocator = std::make_unique<AssetLocator> (std::move (files));
        const auto wallpaper = WallpaperParser::parse (JSON ("scene.json"), project);
        return wallpaper->as<Scene> ()->camera.configuration.hasPaths;
    };
    const JSON transforms = JSON::array ({JSON {{"disabled", true}, {"eye", "17 -23 0"}}});
    const JSON segment {{"transforms", transforms}};
    REQUIRE (parse (JSON::array ({"cameras/pose.json"}), JSON::array ({segment})));
    REQUIRE_FALSE (parse (JSON::array ({"cameras/pose.json", 7}), JSON::array ({segment})));
    REQUIRE_FALSE (parse (JSON::array ({"cameras/pose.json"}),
                         JSON::array ({JSON {{"disabled", true}, {"transforms", transforms}}})));
    REQUIRE_FALSE (parse (JSON::array ({"cameras/pose.json"}),
                         JSON::array ({JSON {{"transforms", JSON::array ()}}, segment})));
    REQUIRE_FALSE (parse (JSON::array (), JSON::array ({segment})));
}

TEST_CASE ("Camera transforms copy partial input and preserve omitted pose fields", "[scene][camera][script]") {
    using WallpaperEngine::Render::Camera;
    Camera::Transforms state {{{0, 0, 500}, {0, 0, 0}, {0, 1, 0}}};
    glm::vec3 eye (80, 40, 500);
    glm::vec3 center (80, 40, 0);
    state = Camera::updatedTransforms (state, &eye, &center, nullptr, nullptr);
    eye.z = 3000;
    center.z = -100;
    state = Camera::updatedTransforms (state, nullptr, &center, nullptr, nullptr);
    REQUIRE (state.pose.eye == glm::vec3 (80, 40, 500));
    REQUIRE (state.pose.center == glm::vec3 (80, 40, -100));
    REQUIRE (state.pose.up == glm::vec3 (0, 1, 0));
    REQUIRE (state.zoom == 1.0f);
    // Actual 2.8 full/partial runtime fixture gives these projected landmarks;
    // an init-only call is overwritten by the native loader (script phase gate).
    const auto vp = Camera::makeProjectionForTransforms (1280, 960, 50, 100, 1000, state, false)
        * Camera::renderLookAtForTransforms (state.pose, false);
    const auto screen = [&] (glm::vec3 point) {
        const auto clip = vp * glm::vec4 (point, 1);
        return (glm::vec2 (clip) / clip.w * 0.5f + 0.5f) * glm::vec2 (1280, 960);
    };
    const auto rectangle = screen ({-80, 60, 0});
    const auto particle = screen ({-60, -40, 0});
    REQUIRE (std::abs (rectangle.x - 310.0f) < 1.0f);
    REQUIRE (std::abs (rectangle.y - 438.5f) < 1.0f);
    REQUIRE (std::abs (particle.x - 351.3333f) < 1.0f);
    REQUIRE (std::abs (particle.y - 644.1111f) < 1.0f);
}

TEST_CASE ("Camera up-only updates rotate the native perspective landmarks", "[scene][camera][script]") {
    using WallpaperEngine::Render::Camera;
    Camera::Transforms state {{{0, 0, 500}, {0, 0, 0}, {0, 1, 0}}};
    const glm::vec3 up (1, 0, 0);
    state = Camera::updatedTransforms (state, nullptr, nullptr, &up, nullptr);
    const auto vp = Camera::makeProjectionForTransforms (1280, 960, 50, 100, 1000, state, false)
        * Camera::renderLookAtForTransforms (state.pose, false);
    const auto clip = vp * glm::vec4 (-80, 60, 0, 1);
    const auto screen = (glm::vec2 (clip) / clip.w * 0.5f + 0.5f) * glm::vec2 (1280, 960);
    // Native rectangle rotates from center (474.5,356) to (516,644.5).
    REQUIRE (std::abs (screen.x - 516.0f) < 1.0f);
    REQUIRE (std::abs (screen.y - 644.5f) < 1.0f);
    const float zoom = 1.5f;
    const auto zoomed = Camera::updatedTransforms (state, nullptr, nullptr, nullptr, &zoom);
    REQUIRE (Camera::makeProjectionForTransforms (1280, 960, 50, 100, 1000, state, false)
        == Camera::makeProjectionForTransforms (1280, 960, 50, 100, 1000, zoomed, false, 1.25f));
}

TEST_CASE ("Script orthographic camera changes basis and zooms about the canvas center",
           "[scene][camera][script]") {
    using WallpaperEngine::Render::Camera;
    const glm::vec2 output (1280, 960);
    const glm::vec3 centeredMarker (-80, 50, 0); // Authored marker (80,70), canvas320x240.
    const auto project = [&] (const Camera::Transforms& state, float authoredZoom = 1.0f) {
        const auto vp = Camera::makeProjectionForTransforms (
            320, 240, 50, 0.1f, 1000, state, true, authoredZoom)
            * Camera::renderLookAtForTransforms (state.pose, true);
        const auto clip = vp * glm::vec4 (centeredMarker, 1);
        return (glm::vec2 (clip) / clip.w * 0.5f + 0.5f) * output;
    };
    const Camera::Transforms translated {{{20, -10, 0}, {20, -10, -1}, {0, 1, 0}}};
    const auto translatedScreen = project (translated);
    // Native translate fixture: (239.5,639.5), not an eye-cancelled view.
    REQUIRE (std::abs (translatedScreen.x - 239.5f) < 0.6f);
    REQUIRE (std::abs (translatedScreen.y - 639.5f) < 0.6f);
    Camera::Transforms zoomed {{{0, 0, 0}, {0, 0, -1}, {0, 1, 0}}, 1.5f};
    const auto zoomedScreen = project (zoomed);
    REQUIRE (std::abs (zoomedScreen.x - 159.5f) < 0.6f);
    REQUIRE (std::abs (zoomedScreen.y - 779.5f) < 0.6f);
    const auto authoredZoomScreen = project (zoomed, 1.25f);
    REQUIRE (std::abs (authoredZoomScreen.x - 39.5f) < 0.6f);
    REQUIRE (std::abs (authoredZoomScreen.y - 854.5f) < 0.6f);
    const auto projection = Camera::makeProjectionForTransforms (320, 240, 50, 100, 1000, zoomed, true);
    for (float depth : {-1500.0f, 1500.0f}) {
        const auto clip = projection * glm::vec4 (0, 0, depth, 1);
        REQUIRE (std::abs (clip.z / clip.w) < 1.0f);
    }
    // Native raw view remains separate from its GL drawing basis.
    const auto native = glm::lookAt (translated.pose.eye, translated.pose.center, translated.pose.up);
    const auto flip = glm::scale (glm::mat4 (1), glm::vec3 (1, -1, 1));
    REQUIRE (Camera::renderLookAtForTransforms (translated.pose, true) == flip * native * flip);
}

TEST_CASE ("Camera API parser copies permissive vectors without coercing components", "[scene][camera][script]") {
    using WallpaperEngine::Scripting::readSceneCameraTransforms;
    JSRuntime* runtime = JS_NewRuntime ();
    REQUIRE (runtime);
    JSContext* context = JS_NewContext (runtime);
    REQUIRE (context);
    WallpaperEngine::Data::Utils::ScopeGuard cleanup ([&] {
        JS_FreeContext (context);
        JS_FreeRuntime (runtime);
    });
    const std::string source = "({eye:{x:80,y:'40',z:500},center:17,up:null,zoom:1.5})";
    JSValue value = JS_Eval (context, source.c_str (), source.size (), "<camera-fields>", JS_EVAL_TYPE_GLOBAL);
    REQUIRE_FALSE (JS_IsException (value));
    auto parsed = readSceneCameraTransforms (context, value);
    REQUIRE (parsed);
    REQUIRE (parsed->eye == glm::vec3 (80, 0, 500));
    REQUIRE_FALSE (parsed->center);
    REQUIRE_FALSE (parsed->up);
    REQUIRE (parsed->zoom == 1.5f);
    JS_FreeValue (context, value);
    REQUIRE (parsed->eye->z == 500.0f); // Result outlives the JS input.
    const std::string ordered =
        "(()=>{let eye={x:1,y:2,z:3};return {eye, get center(){eye.x=9;return null;},zoom:'2'};})()";
    value = JS_Eval (context, ordered.c_str (), ordered.size (), "<camera-getter-order>", JS_EVAL_TYPE_GLOBAL);
    REQUIRE_FALSE (JS_IsException (value));
    parsed = readSceneCameraTransforms (context, value);
    JS_FreeValue (context, value);
    REQUIRE (parsed);
    REQUIRE (parsed->eye == glm::vec3 (9, 2, 3));
    REQUIRE_FALSE (parsed->zoom);
    const std::string throwing = "({eye:{get x(){throw new Error('field failure');}}})";
    value = JS_Eval (context, throwing.c_str (), throwing.size (), "<camera-getter-error>", JS_EVAL_TYPE_GLOBAL);
    REQUIRE_FALSE (JS_IsException (value));
    REQUIRE_FALSE (readSceneCameraTransforms (context, value));
    JS_FreeValue (context, value);
    JSValue exception = JS_GetException (context);
    REQUIRE_FALSE (JS_IsNull (exception));
    JS_FreeValue (context, exception);
}
