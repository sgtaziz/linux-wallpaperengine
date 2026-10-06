#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/WallpaperParser.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Scripting/ScriptDispatchOrder.h"

#include <algorithm>
#include <memory>

#include <string>
#include <vector>

using namespace WallpaperEngine::Scripting;

namespace {
// Registers scripts in the given (Linux creation) order and returns the
// dispatch walk.
struct Registration {
    std::string name;
    int64_t objectIndex;
    ScriptPlacement placement;
};

std::vector<std::string> dispatch (const std::vector<Registration>& registrations) {
    ScriptDispatchOrder order;
    size_t queueOrder = 0;
    for (const auto& registration : registrations)
        order.insert (registration.name, ScriptDispatchRank {.objectIndex = registration.objectIndex,
                                                             .placement = registration.placement,
                                                             .queueOrder = queueOrder++});
    return order.snapshot ();
}

ScriptPlacement own (const char* bindingName) { return ScriptPlacement {.key = nativeScriptJsonKey (bindingName)}; }
} // namespace

TEST_CASE ("Script callbacks walk objects in array order, not lexical key order", "[script][order]") {
    // The w27 fixture's tile C: object 10 alpha writes shared.c = 1, object 20
    // color reads it, object 30 angles writes 2. Native registers 10, 20, 30, so
    // the reader sees 1 each frame; the former lexical walk ran alpha_10,
    // angles_30, color_20 and the reader saw 2.
    const auto walk = dispatch ({{"alpha_10", 0, own ("alpha")}, {"color_20", 1, own ("color")},
                                 {"angles_30", 2, own ("angles")}});
    REQUIRE (walk == std::vector<std::string> {"alpha_10", "color_20", "angles_30"});
}

TEST_CASE ("Script callbacks of one object follow byte-sorted JSON keys", "[script][order]") {
    // Tile S: one image registers its own bindings in the Linux declaration
    // order (scale, color, brightness). jsoncpp's sorted map gives brightness,
    // color, scale, so color reads the brightness writer's value.
    const auto walk = dispatch ({{"scale_40", 3, own ("scale")}, {"color_40", 3, own ("color")},
                                 {"brightness_40", 3, own ("brightness")}});
    REQUIRE (walk == std::vector<std::string> {"brightness_40", "color_40", "scale_40"});
    // 2896906752 / 2955378002: alpha runs before origin on the same layer.
    REQUIRE (dispatch ({{"origin_7", 0, own ("origin")}, {"alpha_7", 0, own ("alpha")}})
             == std::vector<std::string> {"alpha_7", "origin_7"});
    // Bytes, not case-folded order: "parallaxDepth" sorts after "padding".
    REQUIRE (dispatch ({{"parallaxDepth_8", 0, own ("parallaxDepth")}, {"padding_8", 0, own ("padding")}})
             == std::vector<std::string> {"padding_8", "parallaxDepth_8"});
}

TEST_CASE ("Text script bindings sort under their JSON key names", "[script][order]") {
    REQUIRE (nativeScriptJsonKey ("pointSize") == "pointsize");
    REQUIRE (nativeScriptJsonKey ("backgroundColor") == "backgroundcolor");
    REQUIRE (nativeScriptJsonKey ("parallaxDepth") == "parallaxDepth");
    // "pointsize" < "text" while "padding" < "pointsize" by bytes.
    REQUIRE (dispatch ({{"text_5", 0, own ("text")}, {"pointSize_5", 0, own ("pointSize")},
                        {"padding_5", 0, own ("padding")}})
             == std::vector<std::string> {"padding_5", "pointSize_5", "text_5"});
}

TEST_CASE ("General settings and loader phases precede an object's later scripts", "[script][order]") {
    const auto walk = dispatch ({
        // Linux creates an image's own bindings, then animation layers, then
        // its base material, then effects.
        {"origin_1", 0, own ("origin")},
        {"imageAnimationLayer0_object1_rate", 0, {.phase = ScriptPhase::AnimationLayers, .key = "rate"}},
        {"imageMaterial_object1_pass0_constant_color", 0, {.phase = ScriptPhase::BeforeLoad, .key = "color"}},
        {"imageEffect0_object1_visible", 0, {.phase = ScriptPhase::AfterOwn, .group = 0, .subgroup = 0, .key = "visible"}},
        {"imageEffect0_object1_override0_constant_b", 0, {.phase = ScriptPhase::AfterOwn, .subgroup = 1, .key = "b"}},
        {"imageEffect0_object1_override0_constant_a", 0, {.phase = ScriptPhase::AfterOwn, .subgroup = 1, .key = "a"}},
        // The scene general settings are queued before objects in Linux too,
        // but their rank alone must put them first.
        {"general_fov", ScriptDispatchRank::kGeneral, {.key = "fov"}},
        {"general_bloom", ScriptDispatchRank::kGeneral, {.key = "bloom"}},
    });
    REQUIRE (walk == std::vector<std::string> {
        "general_bloom", "general_fov",
        "imageMaterial_object1_pass0_constant_color", "imageAnimationLayer0_object1_rate", "origin_1",
        "imageEffect0_object1_visible", "imageEffect0_object1_override0_constant_a",
        "imageEffect0_object1_override0_constant_b"});
}

TEST_CASE ("Array order decides across objects even against id order", "[script][order]") {
    // The w27 fixture's tile C2: array order 90, 80, 70 (W1, reader, W2).
    // Native follows the array (reader sees 1, green); an id-ordered rank or
    // the former lexical walk (alpha_90, angles_70, color_80) gives 2.
    const auto walk = dispatch ({{"alpha_90", 5, own ("alpha")}, {"color_80", 6, own ("color")},
                                 {"angles_70", 7, own ("angles")}});
    REQUIRE (walk == std::vector<std::string> {"alpha_90", "color_80", "angles_70"});
}

TEST_CASE ("Image scripts register material, animation layers, own keys, then effects by index", "[script][order]") {
    const auto effect = [] (uint32_t index, uint32_t pass, const char* key) {
        return ScriptPlacement {.phase = ScriptPhase::AfterOwn, .group = index, .subgroup = pass, .key = key};
    };
    const auto walk = dispatch ({
        {"own_alpha", 0, own ("alpha")},
        {"effect1_visible", 0, effect (1, 0, "visible")},
        {"effect0_pass1_b", 0, effect (0, 2, "b")},
        {"effect0_pass0_z", 0, effect (0, 1, "z")},
        {"effect0_visible", 0, effect (0, 0, "visible")},
        {"layer1_rate", 0, {.phase = ScriptPhase::AnimationLayers, .group = 1, .key = "rate"}},
        {"layer0_visible", 0, {.phase = ScriptPhase::AnimationLayers, .group = 0, .key = "visible"}},
        {"layer0_blend", 0, {.phase = ScriptPhase::AnimationLayers, .group = 0, .key = "blend"}},
        {"material_pass0_color", 0, {.phase = ScriptPhase::BeforeLoad, .group = 0, .key = "color"}},
        {"effect0_pass0_a", 0, effect (0, 1, "a")},
    });
    REQUIRE (walk == std::vector<std::string> {
        "material_pass0_color", "layer0_blend", "layer0_visible", "layer1_rate", "own_alpha",
        "effect0_visible", "effect0_pass0_a", "effect0_pass0_z", "effect0_pass1_b", "effect1_visible"});
}

TEST_CASE ("Text own keys precede its effect constants", "[script][order]") {
    const auto walk = dispatch ({
        {"textEffect0_object4_pass0_material0_constant_a", 2,
         {.phase = ScriptPhase::AfterOwn, .group = 0, .subgroup = 1, .key = "a"}},
        {"text_4", 2, own ("text")}, {"alpha_4", 2, own ("alpha")},
    });
    REQUIRE (walk == std::vector<std::string> {"alpha_4", "text_4", "textEffect0_object4_pass0_material0_constant_a"});
}

TEST_CASE ("Scripts created after the scene loaded append in creation order", "[script][order]") {
    ScriptDispatchOrder order;
    order.insert ("visible_900", {.objectIndex = ScriptDispatchRank::kAppended, .placement = own ("visible"), .queueOrder = 5});
    order.insert ("alpha_3", {.objectIndex = 1, .placement = own ("alpha"), .queueOrder = 1});
    order.insert ("alpha_900", {.objectIndex = ScriptDispatchRank::kAppended, .placement = own ("alpha"), .queueOrder = 6});
    order.insert ("origin_2", {.objectIndex = 0, .placement = own ("origin"), .queueOrder = 0});
    REQUIRE (order.snapshot () == std::vector<std::string> {"origin_2", "alpha_3", "visible_900", "alpha_900"});
    // A layer created during the update walk appends behind the walked list,
    // so the walk can continue into it in the same frame (follows
    // the live std::list).
    const auto walked = order.snapshot ();
    order.insert ("color_901", {.objectIndex = ScriptDispatchRank::kAppended, .placement = own ("color"), .queueOrder = 7});
    REQUIRE (order.size () == walked.size () + 1);
    REQUIRE (std::equal (walked.begin (), walked.end (), order.snapshot ().begin ()));
    order.erase ("color_901");
    // Removal unlinks a script and keeps the others' order (native list erase).
    order.erase ("alpha_3");
    REQUIRE (order.snapshot () == std::vector<std::string> {"origin_2", "visible_900", "alpha_900"});
    order.clear ();
    REQUIRE (order.snapshot ().empty ());
}

TEST_CASE ("Parsed scenes keep the authored objects array order the rank uses", "[script][order][parser]") {
    using WallpaperEngine::Assets::AssetLocator;
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Scene;
    using WallpaperEngine::Data::Parsers::WallpaperParser;
    using WallpaperEngine::FileSystem::Container;
    // CScene::sceneObjectIndex reads SceneData::objects; the C2 tile's ids run
    // against the array (90, 80, 70), which must survive parsing.
    auto files = std::make_unique<Container> ();
    files->getVFS ().add ("scene.json", R"({
      "camera":{"center":"0 0 -1","eye":"0 0 0","up":"0 1 0"},
      "general":{"orthogonalprojection":{"width":100,"height":100}},
      "objects":[{"id":90,"name":"w1","origin":"0 0 0"},{"id":80,"name":"r","origin":"0 0 0"},
                 {"id":70,"name":"w2","origin":"0 0 0"}]
    })");
    Project project {};
    project.type = Project::Type_Scene;
    project.assetLocator = std::make_unique<AssetLocator> (std::move (files));
    const auto wallpaper = WallpaperParser::parse (JSON ("scene.json"), project);
    const auto* scene = dynamic_cast<const Scene*> (wallpaper.get ());
    REQUIRE (scene != nullptr);
    std::vector<int> ids;
    for (const auto& object : scene->objects) ids.push_back (object->id);
    REQUIRE (ids == std::vector<int> {90, 80, 70});
}
