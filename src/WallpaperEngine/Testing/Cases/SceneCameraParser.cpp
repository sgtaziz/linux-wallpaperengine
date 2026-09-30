#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/WallpaperParser.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Render/Wallpapers/SceneDependencies.h"
#include "WallpaperEngine/Render/Camera.h"
#include "WallpaperEngine/Render/Wallpapers/SceneCursor.h"
#include "WallpaperEngine/Render/Wallpapers/SceneTransform.h"
#include "WallpaperEngine/Input/MouseInput.h"
#include "WallpaperEngine/Render/Shaders/ShaderUnit.h"

#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <memory>
#include <string>
#include <tuple>

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
                 "nearz":0.25,"farz":250,"fov":75,},
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
    // a translated eye, including positive z, as well as negative near.
    const glm::vec3 movedEye { 10.0f, -5.0f, 1.0f };
    const glm::mat4 movedView = glm::lookAt (
        movedEye, movedEye + glm::vec3 (0.0f, 0.0f, -1.0f), glm::vec3 (0.0f, 1.0f, 0.0f));
    const auto negativeNear = Camera::makeOrthogonalProjectionForScene (
        1920.0f, 1080.0f, -2.0f, 10000.0f, movedEye);
    const glm::vec4 movedClip = negativeNear * movedView * imagePlane;
    REQUIRE (movedClip.z >= -movedClip.w);
    REQUIRE (movedClip.z <= movedClip.w);
    REQUIRE (negativeNear[2][2] != baseline[2][2]);

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
    REQUIRE (std::abs (crop->y - 20.0f) < 1e-4f);
    const auto shifted = Camera::makeOrthogonalProjectionForScene (
        100.0f, 80.0f, 0.0f, 1000.0f, {10.0f, -5.0f, 0.0f});
    const auto moved = cursorWorldPosition ({0.25f, 0.75f}, shifted, 100.0f, 80.0f);
    REQUIRE (moved.has_value ());
    REQUIRE (std::abs (moved->x - 15.0f) < 1e-4f);
    REQUIRE (std::abs (moved->y - 15.0f) < 1e-4f);
    const glm::mat4 layer = glm::translate (glm::mat4 (1.0f), {50.0f, 40.0f, 0.0f})
        * glm::rotate (glm::mat4 (1.0f), 0.5f, {0.0f, 0.0f, 1.0f});
    const glm::vec3 point = glm::vec3 (layer * glm::vec4 (8.0f, -3.0f, 0.0f, 1.0f));
    const auto local = cursorHitLocalPosition (point, layer, {40.0f, 20.0f});
    REQUIRE (local.has_value ());
    REQUIRE (std::abs (local->x - 28.0f) < 1e-4f);
    REQUIRE (std::abs (local->y - 7.0f) < 1e-4f);
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
