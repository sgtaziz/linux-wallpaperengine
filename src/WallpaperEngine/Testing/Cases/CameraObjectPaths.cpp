#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/WallpaperParser.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Render/Camera.h"
#include "WallpaperEngine/Render/CameraObjectPaths.h"
#include "WallpaperEngine/Render/Wallpapers/SceneTransform.h"

#include <random>
#include <set>
#include <string>
#include <vector>

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;
using WallpaperEngine::Data::Model::SceneCamera;

namespace {
// Constant-pose path item: eye.x = x on every frame, so a tick's pose names the item.
std::string constantItem (float x, const char* mode, int length = 10) {
    const std::string value = std::to_string (x);
    const std::string last = std::to_string (length);
    const auto channel = [&] (const std::string& v) {
        return "[{\"frame\":0,\"value\":" + v + "},{\"frame\":" + last + ",\"value\":" + v + "}]";
    };
    return "{\"options\":{\"fps\":10,\"length\":" + last + ",\"mode\":\"" + mode + "\"},"
           "\"center\":{\"c0\":" + channel (value) + ",\"c1\":" + channel ("0") + ",\"c2\":" + channel ("0") + "},"
           "\"eye\":{\"c0\":" + channel (value) + ",\"c1\":" + channel ("0") + ",\"c2\":" + channel ("5") + "},"
           "\"up\":{\"c0\":" + channel ("0") + ",\"c1\":" + channel ("1") + ",\"c2\":" + channel ("0") + "}}";
}

struct Fixture {
    std::unique_ptr<Data::Model::Wallpaper> wallpaper;
    const SceneCamera* camera = nullptr;
};

Fixture parseScene (const std::string& pathsJson, const std::string& queueMode) {
    auto files = std::make_unique<FileSystem::Container> ();
    files->getVFS ().add ("scene.json", R"({
      "camera":{"center":"0 0 0","eye":"0 0 1","up":"0 1 0"},
      "general":{"fov":60},
      "objects":[{"id":7,"name":"cam","camera":"default","path":"paths.json","fov":45,
                  "origin":"1 2 3")" + std::string (queueMode.empty () ? "" : ",\"queuemode\":\"" + queueMode + "\"")
                  + "}]}");
    files->getVFS ().add ("paths.json", pathsJson);
    Data::Model::Project project {};
    project.type = Data::Model::Project::Type_Scene;
    project.assetLocator = std::make_unique<Assets::AssetLocator> (std::move (files));
    Fixture result;
    result.wallpaper = Data::Parsers::WallpaperParser::parse (Data::JSON::JSON ("scene.json"), project);
    const auto* scene = dynamic_cast<const Data::Model::Scene*> (result.wallpaper.get ());
    REQUIRE (scene != nullptr);
    result.camera = scene->objects.front ()->as<SceneCamera> ();
    return result;
}

std::string pathsFile (const std::vector<std::string>& items) {
    std::string out = "{\"paths\":[";
    for (size_t i = 0; i < items.size (); ++i) out += (i ? "," : "") + items[i];
    return out + "]}";
}

CameraObjectPathFrame defaultsFrame () { return {{0, 0, 0}, {0, 0, -5}, {0, 1, 0}}; }

// Which constant item (by eye.x / 10) is current after each 0.25 s tick.
std::vector<int> runTicks (CameraObjectPathState& state, bool sequential, std::mt19937& rng, int ticks) {
    std::vector<int> order;
    for (int tick = 0; tick < ticks; ++tick) {
        const auto frame = advanceCameraObjectPaths (state, sequential, rng, 0.25f, false, defaultsFrame ());
        REQUIRE (frame.has_value ());
        order.push_back (static_cast<int> (frame->eye.x / 10.0f + 0.5f));
    }
    return order;
}
}

TEST_CASE ("Camera object paths parse native items and queue mode", "[scene][camera][paths]") {
    const auto invalid = R"({"options":{"fps":0,"length":10,"mode":"single"}})";
    const auto fixture = parseScene (pathsFile ({constantItem (0, "single"), invalid, constantItem (10, "single"),
        "[]"}), "sequential");
    // The fps=0 item and the non-object entry are dropped.
    REQUIRE (fixture.camera->paths.size () == 2);
    REQUIRE (fixture.camera->sequentialQueue);
    const auto& first = fixture.camera->paths[0];
    REQUIRE (first.timeline.mode == Data::Model::PropertyAnimation::Mode::Single);
    REQUIRE (first.timeline.duration () == Catch::Approx (1.0f));
    REQUIRE (first.vectors[1].channels[2].size () == 2);
    REQUIRE (first.zoom.channels[0].empty ());
    // Native: queuemode is random (0) unless exactly "sequential".
    REQUIRE_FALSE (parseScene (pathsFile ({constantItem (0, "single")}), "").camera->sequentialQueue);
    REQUIRE_FALSE (parseScene (pathsFile ({constantItem (0, "single")}), "random").camera->sequentialQueue);
    REQUIRE_FALSE (parseScene (pathsFile ({constantItem (0, "single")}), "Sequential").camera->sequentialQueue);
}

TEST_CASE ("Sequential camera object queue advances only when a single-mode path finishes", "[scene][camera][paths]") {
    const auto fixture = parseScene (pathsFile (
        {constantItem (0, "single"), constantItem (10, "single"), constantItem (20, "single")}), "sequential");
    CameraObjectPathState state (fixture.camera->paths);
    std::mt19937 rng (1);
    // 1 s paths at 0.25 s ticks: the finishing tick (time == duration) still shows the
    // old path; the next tick rewinds it, picks the successor and advances it.
    REQUIRE (runTicks (state, true, rng, 14)
        == std::vector<int> {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0});
    REQUIRE (rng () == std::mt19937 (1) ());

    // Looping and start-paused paths never finish, so the queue never advances.
    for (const char* mode : {"loop", "mirror"}) {
        const auto looping = parseScene (pathsFile ({constantItem (0, mode), constantItem (10, "single")}), "sequential");
        CameraObjectPathState loopState (looping.camera->paths);
        const auto order = runTicks (loopState, true, rng, 40);
        REQUIRE (std::set<int> (order.begin (), order.end ()) == std::set<int> {0});
    }
    auto paused = parseScene (pathsFile ({constantItem (0, "single"), constantItem (10, "single")}), "sequential");
    CameraObjectPathState pausedState (paused.camera->paths);
    pausedState.paths[0].timeline.paused = true;
    const auto order = runTicks (pausedState, true, rng, 20);
    REQUIRE (std::set<int> (order.begin (), order.end ()) == std::set<int> {0});
}

TEST_CASE ("Random camera object queue deals each path once per bag without an immediate repeat",
           "[scene][camera][paths]") {
    std::vector<std::string> items;
    for (int index = 0; index < 4; ++index) items.push_back (constantItem (static_cast<float> (index * 10), "single", 1));
    const auto fixture = parseScene (pathsFile (items), "random");
    CameraObjectPathState state (fixture.camera->paths);
    std::mt19937 rng (5489);
    // Length 1 at fps 10 finishes on its first tick, so every call after the first is a new pick.
    std::vector<int> picks;
    for (int pick = 0; pick < 24; ++pick) {
        const auto frame = advanceCameraObjectPaths (state, false, rng, 0.25f, false, defaultsFrame ());
        picks.push_back (static_cast<int> (frame->eye.x / 10.0f + 0.5f));
    }
    for (size_t bag = 0; bag + 4 <= picks.size (); bag += 4)
        REQUIRE (std::set<int> (picks.begin () + bag, picks.begin () + bag + 4).size () == 4);
    for (size_t index = 1; index < picks.size (); ++index) REQUIRE (picks[index] != picks[index - 1]);
    // Exactly one mt19937 word per pick (power-of-two and small queue sizes never reject here).
    std::mt19937 reference (5489);
    reference.discard (picks.size ());
    REQUIRE (rng () == reference ());
}

TEST_CASE ("A lone random camera object path repeats", "[scene][camera][paths]") {
    const auto fixture = parseScene (pathsFile ({constantItem (0, "single", 1)}), "random");
    CameraObjectPathState state (fixture.camera->paths);
    std::mt19937 rng (7);
    const auto order = runTicks (state, false, rng, 8);
    REQUIRE (order == std::vector<int> (8, 0));
    REQUIRE (state.hasLast);
}

TEST_CASE ("Camera object path pose round-trips through the object transform", "[scene][camera][paths]") {
    const glm::vec3 eye (3.0f, -2.0f, 7.0f), center (-1.0f, 0.5f, 2.0f), up (0.0f, 1.0f, 0.0f);
    const auto fixture = parseScene (pathsFile ({constantItem (0, "single")}), "sequential");
    fixture.camera->origin->value->update (eye, Data::Model::DynamicValue::Script);
    fixture.camera->groupAngles->value->update (cameraObjectAnglesForPose (eye, center, up),
        Data::Model::DynamicValue::Script);
    const auto resolved = Wallpapers::resolveSceneTransform (*fixture.camera, [] (int) -> const Data::Model::Object* {
        return nullptr;
    });
    const auto pose = Camera::poseForWorldCamera (resolved.authoredMatrix);
    const auto forward = glm::normalize (center - eye);
    REQUIRE (glm::length (pose.eye - eye) < 1e-4f);
    REQUIRE (glm::length (glm::normalize (pose.center - pose.eye) - forward) < 1e-4f);
    // lookAt orthogonalises the requested up against the view direction.
    REQUIRE (glm::length (pose.up - glm::normalize (up - forward * glm::dot (up, forward))) < 5e-4f);
}

TEST_CASE ("Camera object path zoom and fov default to native values per projection", "[scene][camera][paths]") {
    auto item = constantItem (0, "single");
    item.insert (item.size () - 1, ",\"fov\":[{\"frame\":0,\"value\":30},{\"frame\":10,\"value\":30}],"
                 "\"zoom\":[{\"frame\":0,\"value\":2},{\"frame\":10,\"value\":2}]");
    const auto fixture = parseScene (pathsFile ({item, constantItem (10, "single")}), "sequential");
    std::mt19937 rng (1);
    CameraObjectPathState perspective (fixture.camera->paths);
    auto frame = advanceCameraObjectPaths (perspective, true, rng, 0.25f, false, defaultsFrame ());
    // Perspective writes only fov; the item's zoom keeps its constructor default of 1.
    REQUIRE (frame->fov == Catch::Approx (30.0f));
    REQUIRE (frame->zoom == 1.0f);
    CameraObjectPathState orthogonal (fixture.camera->paths);
    frame = advanceCameraObjectPaths (orthogonal, true, rng, 0.25f, true, defaultsFrame ());
    REQUIRE (frame->zoom == Catch::Approx (2.0f));
    REQUIRE (frame->fov == 50.0f);
    // A path without a fov track reports the native default 50, not the object's own fov (45).
    for (int tick = 0; tick < 5; ++tick) frame = advanceCameraObjectPaths (perspective, true, rng, 0.25f, false, defaultsFrame ());
    REQUIRE (perspective.current == 1);
    REQUIRE (frame->fov == 50.0f);
}
