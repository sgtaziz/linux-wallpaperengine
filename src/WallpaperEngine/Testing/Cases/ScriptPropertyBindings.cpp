#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/Builders/UserSettingBuilder.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Data/Parsers/WallpaperParser.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Scripting/ScriptPropertyBindings.h"
#include "WallpaperEngine/Render/Wallpapers/SceneBloomConstants.h"
#include "WallpaperEngine/Render/Wallpapers/SceneTransform.h"
#include <catch2/catch_approx.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using WallpaperEngine::Data::Builders::UserSettingBuilder;
using WallpaperEngine::Data::Model::DynamicValue;
using WallpaperEngine::Data::Model::Image;
using WallpaperEngine::Data::Model::ImageData;
using WallpaperEngine::Data::Model::ObjectData;
using WallpaperEngine::Data::Model::Particle;
using WallpaperEngine::Data::Model::ParticleData;
using WallpaperEngine::Data::Model::Text;
using WallpaperEngine::Data::Model::TextData;
using WallpaperEngine::Scripting::scriptPropertyBindings;

namespace {
ObjectData baseObject () {
    ObjectData base {};
    base.id = 42;
    base.name = "typed object";
    base.origin = UserSettingBuilder::fromValue (glm::vec3 (0.0f));
    base.groupScale = UserSettingBuilder::fromValue (glm::vec3 (9.0f));
    base.groupAngles = UserSettingBuilder::fromValue (glm::vec3 (8.0f));
    base.groupVisible = UserSettingBuilder::fromValue (false);
    base.parallaxDepth = UserSettingBuilder::fromValue (glm::vec2 (0.0f));
    return base;
}

template <size_t N>
DynamicValue* binding (const std::array<WallpaperEngine::Scripting::ScriptPropertyBinding, N>& bindings,
		       const std::string& name) {
    const auto it = std::find_if (bindings.begin (), bindings.end (), [&] (const auto& entry) {
	return name == entry.name;
    });
    return it == bindings.end () ? nullptr : &it->value;
}

template <size_t N>
std::set<std::string> names (const std::array<WallpaperEngine::Scripting::ScriptPropertyBinding, N>& bindings) {
    std::set<std::string> result;
    for (const auto& entry : bindings) result.insert (entry.name);
    return result;
}
} // namespace

TEST_CASE ("MODEL script transforms update the authored world matrix used by collision bounds", "[script][binding][collision-model]") {
    using WallpaperEngine::Data::Model::SceneModel;
    using WallpaperEngine::Data::Model::SceneModelData;
    using WallpaperEngine::Render::Wallpapers::resolveSceneTransform;
    SceneModel model (baseObject (), SceneModelData {});
    const auto bindings = scriptPropertyBindings (model);
    REQUIRE (names (bindings) == std::set<std::string> {"origin", "scale", "angles", "visible"});
    binding (bindings, "origin")->update (glm::vec3 (100, 200, 3), DynamicValue::Script);
    binding (bindings, "scale")->update (glm::vec3 (2, 3, 4), DynamicValue::Script);
    binding (bindings, "angles")->update (glm::vec3 (0, 0, glm::half_pi<float> ()), DynamicValue::Script);
    binding (bindings, "visible")->update (true, DynamicValue::Script);
    const auto world = resolveSceneTransform (model, [] (int) -> const WallpaperEngine::Data::Model::Object* {
        return nullptr;
    }).authoredMatrix;
    const glm::vec3 point (world * glm::vec4 (1, 0, 0, 1));
    REQUIRE (point.x == Catch::Approx (100));
    REQUIRE (point.y == Catch::Approx (202));
    REQUIRE (point.z == Catch::Approx (3));
    REQUIRE (model.groupVisible->value->getBool ());
}

TEST_CASE ("Image script bindings update the typed values consumed by rendering", "[script][binding]") {
    ImageData data {};
    data.scale = UserSettingBuilder::fromValue (glm::vec3 (1.0f));
    data.angles = UserSettingBuilder::fromValue (glm::vec3 (2.0f));
    data.visible = UserSettingBuilder::fromValue (true);
    data.alpha = UserSettingBuilder::fromValue (0.5f);
    data.color = UserSettingBuilder::fromValue (glm::vec3 (1.0f));
    data.brightness = UserSettingBuilder::fromValue (1.0f);
    data.size = UserSettingBuilder::fromValue (glm::vec2 (32.0f, 24.0f));
    Image image (baseObject (), std::move (data));
    const auto bindings = scriptPropertyBindings (image);
    REQUIRE (names (bindings) == std::set<std::string> {"origin", "scale", "angles", "visible",
						       "alpha", "color", "brightness", "parallaxDepth", "size"});
    REQUIRE (binding (bindings, "scale") == image.scale->value.get ());
    REQUIRE (binding (bindings, "scale") != image.groupScale->value.get ());
    REQUIRE (binding (bindings, "visible") == image.visible->value.get ());
    REQUIRE (binding (bindings, "visible") != image.groupVisible->value.get ());
    REQUIRE (binding (bindings, "size") == image.size->value.get ());
    REQUIRE (binding (bindings, "brightness") == image.brightness->value.get ());
    binding (bindings, "brightness")->update (1.2694342f, DynamicValue::Script);
    REQUIRE (image.brightness->value->getFloat () == 1.2694342f);
    binding (bindings, "scale")->update (glm::vec3 (3.0f), DynamicValue::Script);
    REQUIRE (image.scale->value->getVec3 ().x == 3.0f);
    REQUIRE (image.groupScale->value->getVec3 ().x == 9.0f);
    binding (bindings, "size")->update (glm::vec2 (64.0f, 48.0f), DynamicValue::Script);
    REQUIRE (image.size->value->getVec2 () == glm::vec2 (64.0f, 48.0f));
}

TEST_CASE ("Particle and text bindings retain prior names with typed authority", "[script][binding]") {
    ParticleData particleData {};
    particleData.scale = UserSettingBuilder::fromValue (glm::vec3 (1.0f));
    particleData.angles = UserSettingBuilder::fromValue (glm::vec3 (2.0f));
    particleData.visible = UserSettingBuilder::fromValue (true);
    Particle particle (baseObject (), std::move (particleData));
    const auto particleBindings = scriptPropertyBindings (particle);
    REQUIRE (names (particleBindings) == std::set<std::string> {"origin", "scale", "angles", "visible", "parallaxDepth"});
    REQUIRE (binding (particleBindings, "scale") == particle.scale->value.get ());
    REQUIRE (binding (particleBindings, "scale") != particle.groupScale->value.get ());
    binding (particleBindings, "visible")->update (false, DynamicValue::Script);
    REQUIRE_FALSE (particle.visible->value->getBool ());
    REQUIRE_FALSE (particle.groupVisible->value->getBool ());

    TextData textData {};
    textData.text = UserSettingBuilder::fromValue (std::string ("hello"));
    textData.pointSize = UserSettingBuilder::fromValue (24.0f);
    textData.spacing = UserSettingBuilder::fromValue (glm::vec2 (0.0f));
    textData.limitRows = UserSettingBuilder::fromValue (false);
    textData.maxRows = UserSettingBuilder::fromValue (0);
    textData.limitWidth = UserSettingBuilder::fromValue (false);
    textData.maxWidth = UserSettingBuilder::fromValue (0.0f);
    textData.limitUseEllipsis = UserSettingBuilder::fromValue (false);
    textData.opaqueBackground = UserSettingBuilder::fromValue (false);
    textData.backgroundColor = UserSettingBuilder::fromValue (glm::vec3 (0.0f));
    textData.backgroundBrightness = UserSettingBuilder::fromValue (1.0f);
    textData.padding = UserSettingBuilder::fromValue (glm::vec2 (32.0f));
    textData.scale = UserSettingBuilder::fromValue (glm::vec3 (1.0f));
    textData.color = UserSettingBuilder::fromValue (glm::vec3 (1.0f));
    textData.brightness = UserSettingBuilder::fromValue (1.0f);
    textData.alpha = UserSettingBuilder::fromValue (1.0f);
    textData.visible = UserSettingBuilder::fromValue (true);
    Text text (baseObject (), std::move (textData));
    const auto textBindings = scriptPropertyBindings (text);
    REQUIRE (names (textBindings) == std::set<std::string> {"origin", "scale", "angles", "visible",
							   "color", "brightness", "alpha", "pointSize", "spacing", "limitRows", "maxRows",
							   "limitWidth", "maxWidth",
							   "limitUseEllipsis", "opaqueBackground", "backgroundColor", "backgroundBrightness", "padding", "text",
							   "parallaxDepth"});
    REQUIRE (binding (textBindings, "parallaxDepth") == text.parallaxDepth->value.get ());
    REQUIRE (binding (textBindings, "scale") == text.scale->value.get ());
    REQUIRE (binding (textBindings, "scale") != text.groupScale->value.get ());
    REQUIRE (binding (textBindings, "angles") == text.groupAngles->value.get ());
    REQUIRE (binding (textBindings, "spacing") == text.spacing->value.get ());
    binding (textBindings, "spacing")->update (glm::vec2 (2.0f, 4.0f), DynamicValue::Script);
    REQUIRE (text.spacing->value->getVec2 () == glm::vec2 (2.0f, 4.0f));
    REQUIRE (binding (textBindings, "limitRows") == text.limitRows->value.get ());
    binding (textBindings, "limitRows")->update (true, DynamicValue::Script);
    REQUIRE (text.limitRows->value->getBool ());
    REQUIRE (binding (textBindings, "maxRows") == text.maxRows->value.get ());
    binding (textBindings, "maxRows")->update (2, DynamicValue::Script);
    REQUIRE (text.maxRows->value->getInt () == 2);
    REQUIRE (binding (textBindings, "limitWidth") == text.limitWidth->value.get ());
    binding (textBindings, "limitWidth")->update (true, DynamicValue::Script);
    REQUIRE (text.limitWidth->value->getBool ());
    REQUIRE (binding (textBindings, "maxWidth") == text.maxWidth->value.get ());
    binding (textBindings, "maxWidth")->update (120.0f, DynamicValue::Script);
    REQUIRE (text.maxWidth->value->getFloat () == 120.0f);
    REQUIRE (binding (textBindings, "limitUseEllipsis") == text.limitUseEllipsis->value.get ());
    binding (textBindings, "limitUseEllipsis")->update (true, DynamicValue::Script);
    REQUIRE (text.limitUseEllipsis->value->getBool ());
    REQUIRE (binding (textBindings, "opaqueBackground") == text.opaqueBackground->value.get ());
    binding (textBindings, "opaqueBackground")->update (true, DynamicValue::Script);
    REQUIRE (text.opaqueBackground->value->getBool ());
    REQUIRE (binding (textBindings, "backgroundColor") == text.backgroundColor->value.get ());
    binding (textBindings, "backgroundColor")->update (glm::vec3 (1.0f, 0.0f, 0.0f), DynamicValue::Script);
    REQUIRE (text.backgroundColor->value->getVec4 ().r == 1.0f);
    REQUIRE (binding (textBindings, "padding") == text.padding->value.get ());
    binding (textBindings, "padding")->update (glm::vec2 (8.0f, 16.0f), DynamicValue::Script);
    REQUIRE (text.padding->value->getVec2 () == glm::vec2 (8.0f, 16.0f));
    binding (textBindings, "text")->update (std::string ("updated"), DynamicValue::Script);
    REQUIRE (text.text->value->getString () == "updated");
}

TEST_CASE ("Transform-only parent bindings drive the child render matrix", "[script][binding][scene]") {
    using WallpaperEngine::Data::Model::Object;
    using WallpaperEngine::Render::Wallpapers::resolveSceneTransform;
    auto parentData = baseObject ();
    parentData.groupScale = UserSettingBuilder::fromValue (glm::vec3 (1.0f));
    parentData.groupAngles = UserSettingBuilder::fromValue (glm::vec3 (0.0f));
    parentData.groupVisible = UserSettingBuilder::fromValue (true);
    Object parent (std::move (parentData));
    auto childData = baseObject ();
    childData.id = 43;
    childData.parent = parent.id;
    childData.origin = UserSettingBuilder::fromValue (glm::vec3 (50.0f, 0.0f, 0.0f));
    childData.groupScale = UserSettingBuilder::fromValue (glm::vec3 (1.0f));
    childData.groupAngles = UserSettingBuilder::fromValue (glm::vec3 (0.0f));
    childData.groupVisible = UserSettingBuilder::fromValue (true);
    Object child (std::move (childData));
    const auto bindings = scriptPropertyBindings (parent);
    const auto resolve = [&] {
        return resolveSceneTransform (child, [&] (int id) -> const Object* {
            return id == parent.id ? &parent : nullptr;
        });
    };
    REQUIRE (resolve ().origin.x == 50.0f);
    binding (bindings, "angles")->update (glm::vec3 (0.0f, 0.0f, glm::half_pi<float> ()), DynamicValue::Script);
    const auto rotated = resolve ();
    REQUIRE (rotated.origin.x == Catch::Approx (0.0f).margin (0.00001f));
    REQUIRE (rotated.origin.y == Catch::Approx (50.0f));
    binding (bindings, "origin")->update (glm::vec3 (100.0f, 200.0f, 0.0f), DynamicValue::Script);
    REQUIRE (resolve ().origin.y == Catch::Approx (250.0f));
    binding (bindings, "visible")->update (false, DynamicValue::Script);
    REQUIRE_FALSE (resolve ().visible);
}

namespace {
std::map<std::string, DynamicValue*> bindingMap (const std::vector<WallpaperEngine::Scripting::ScriptPropertyBinding>& bindings) {
    std::map<std::string, DynamicValue*> result;
    for (const auto& entry : bindings) result.emplace (entry.name, &entry.value);
    return result;
}
} // namespace

TEST_CASE ("Sound layers queue base transform scripts alongside volume", "[script][binding][audio]") {
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Sound;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    // Reduced from 2915841260: the Music combo selector is a sound-layer
    // scale script whose init stops every track and whose
    // applyUserProperties plays the chosen one.
    Project project {};
    const auto object = ObjectParser::parse (JSON::parse (R"({
        "id":101494,"name":"Theme.mp3","sound":["sounds/theme.mp3"],"startsilent":true,
        "playbackmode":"loop","volume":{"user":"musicvolume","value":0.6},
        "scale":{"script":"export function init(){}\nexport function applyUserProperties(p){}",
                 "value":"1 1 1"}})"), project);
    REQUIRE (object->is<Sound> ());
    const auto& sound = *object->as<Sound> ();
    const auto bindings = bindingMap (scriptPropertyBindings (sound));
    REQUIRE (bindings.size () == 4);
    REQUIRE (bindings.at ("origin") == sound.origin->value.get ());
    REQUIRE (bindings.at ("scale") == sound.groupScale->value.get ());
    REQUIRE (bindings.at ("angles") == sound.groupAngles->value.get ());
    REQUIRE (bindings.at ("volume") == sound.volume->value.get ());
    // "visible" is not in the native sound lookup chain (-> sound
    // table -> base table); a script write stays an expando.
    REQUIRE_FALSE (bindings.contains ("visible"));
    // The queued value carries the authored module, so CSound's registration
    // reaches ScriptEngine::queueScript with a script to run.
    const auto& source = bindings.at ("scale")->getScriptSource ();
    REQUIRE (source.has_value ());
    REQUIRE (source->find ("applyUserProperties") != std::string::npos);
    REQUIRE_FALSE (bindings.at ("volume")->getScriptSource ().has_value ());

    // A sound without an authored volume still binds its transform values.
    const auto silent = ObjectParser::parse (
        JSON::parse (R"({"id":5,"name":"bare","sound":["sounds/a.mp3"]})"), project);
    const auto bare = bindingMap (scriptPropertyBindings (*silent->as<Sound> ()));
    REQUIRE (bare.at ("scale") == silent->as<Sound> ()->groupScale->value.get ());
}

TEST_CASE ("Image model material constants are offered for script queuing", "[script][binding][parser]") {
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Scripting::imageMaterialScriptBindings;
    // Reduced from 2744921335's bar.json: the image's own material carries an
    // audio-driven "color" constant script beside plain constants.
    auto files = std::make_unique<WallpaperEngine::FileSystem::Container> ();
    files->getVFS ().add ("materials/bar.json", R"({"passes":[{"shader":"custom_tint",
        "constantshadervalues":{"Alpha":0.47,
          "color":{"script":"export function update(value){ return value; }","value":"1 1 1"}}}]})");
    files->getVFS ().add ("models/bar.json", R"({"material":"materials/bar.json","autosize":true})");
    Project project {};
    project.assetLocator = std::make_unique<WallpaperEngine::Assets::AssetLocator> (std::move (files));
    const auto object = ObjectParser::parse (JSON {{"id", 180}, {"image", "models/bar.json"}}, project);
    const auto& image = *object->as<Image> ();
    const auto bindings = imageMaterialScriptBindings (image);
    REQUIRE (bindings.size () == 2);
    const auto& constants = image.model->material->passes[0]->constants;
    const auto color = std::find_if (bindings.begin (), bindings.end (), [] (const auto& entry) {
        return entry.key == "pass0_constant_color";
    });
    REQUIRE (color != bindings.end ());
    // The value is the one CPass binds for the base pass, and its owner map
    // is the material handed to scripts as thisObject.
    REQUIRE (&color->value == constants.at ("color")->value.get ());
    REQUIRE (&color->constants == &constants);
    REQUIRE (color->value.getScriptSource ().has_value ());
    // The key keeps the "_constant_color" suffix that preserves alpha on an
    // RGB script result.
    REQUIRE (color->key.ends_with ("_constant_color"));
}

TEST_CASE ("Scene general settings expose scripted and animated fields to the scene queue",
           "[script][binding][scene][parser]") {
    using WallpaperEngine::Assets::AssetLocator;
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Scene;
    using WallpaperEngine::Data::Parsers::WallpaperParser;
    using WallpaperEngine::FileSystem::Container;
    using WallpaperEngine::Scripting::sceneGeneralScriptBindings;

    // Forms taken from installed scenes: bloomstrength script (2134765860),
    // camerashake* scripts (2235507313), hdr strength script with user and
    // scriptproperties (2374921972) and a single-mode zoom animation
    // (3027732033).
    auto files = std::make_unique<Container> ();
    files->getVFS ().add ("scene.json", R"({
      "camera":{"center":"0 0 -1","eye":"0 0 0","up":"0 1 0"},
      "general":{"orthogonalprojection":{"width":1920,"height":1080},
        "bloom":true,
        "bloomstrength":{"script":"export function update(){ return 2.5; }","value":1.0},
        "bloomhdrstrength":{"script":"export function init(v){}","scriptproperties":{"minvalue":1},
                            "user":"hdrstrength","value":6.0},
        "camerashakeamplitude":{"script":"export function update(){ return 0.3; }","value":0.46},
        "clearenabled":true,
        "zoom":{"animation":{"c0":[{"frame":0,"value":1.16},{"frame":60,"value":1}],
                             "options":{"fps":6,"length":60,"mode":"single"}},"value":1.0}},
      "objects":[]})");
    Project project {};
    project.type = Project::Type_Scene;
    project.assetLocator = std::make_unique<AssetLocator> (std::move (files));
    const auto wallpaper = WallpaperParser::parse (JSON ("scene.json"), project);
    const auto* scene = dynamic_cast<const Scene*> (wallpaper.get ());
    REQUIRE (scene != nullptr);
    const auto bindings = bindingMap (sceneGeneralScriptBindings (*scene));

    // Every field kept by the scene model is offered, under its native name.
    for (const char* name : {"clearenabled", "clearcolor", "ambientcolor", "skylightcolor", "camerafade", "bloom",
                             "bloomstrength", "bloomthreshold", "bloomhdrstrength", "bloomhdrthreshold",
                             "bloomhdrfeather", "bloomhdrscatter", "bloomhdriterations", "bloomtint",
                             "cameraparallax", "cameraparallaxamount", "cameraparallaxdelay",
                             "cameraparallaxmouseinfluence", "camerashake", "camerashakeamplitude",
                             "camerashakeroughness", "camerashakespeed", "nearz", "farz", "fov", "zoom",
                             "perspectiveoverridefov"})
        REQUIRE (bindings.contains (name));
    REQUIRE (bindings.size () == 27);

    // The bound values are the ones the renderer and thisScene read.
    REQUIRE (bindings.at ("bloomstrength") == scene->camera.bloom.strength->value.get ());
    REQUIRE (bindings.at ("bloomhdrstrength") == scene->camera.bloom.hdrStrength->value.get ());
    REQUIRE (bindings.at ("camerashakeamplitude") == scene->camera.shake.amplitude->value.get ());
    REQUIRE (bindings.at ("clearenabled") == scene->clearEnabled->value.get ());
    REQUIRE (bindings.at ("zoom") == scene->camera.projection.zoom->value.get ());
    REQUIRE (bindings.at ("bloomstrength")->getScriptSource ().has_value ());
    REQUIRE (bindings.at ("bloomhdrstrength")->getScriptSource ().has_value ());
    REQUIRE (bindings.at ("camerashakeamplitude")->getScriptSource ().has_value ());
    REQUIRE (bindings.at ("zoom")->getAnimation () != nullptr);
    REQUIRE_FALSE (bindings.at ("bloomthreshold")->getScriptSource ().has_value ());
}
TEST_CASE ("Non-HDR bloom passes follow the live general strength and threshold", "[script][scene][bloom]") {
    using WallpaperEngine::Data::Model::ImageEffect;
    using WallpaperEngine::Data::Model::ImageEffectPassOverride;
    using WallpaperEngine::Data::Model::ImageEffectUniquePtr;
    using WallpaperEngine::Render::Wallpapers::applyBloomConstants;
    std::vector<ImageEffectUniquePtr> effects;
    auto effect = std::make_unique<ImageEffect> ();
    for (int pass = 0; pass < 3; ++pass) {
        auto override = std::make_unique<ImageEffectPassOverride> ();
        override->constants.emplace ("bloomstrength", UserSettingBuilder::fromValue (2.0f));
        override->constants.emplace ("bloomthreshold", UserSettingBuilder::fromValue (0.65f));
        override->constants.emplace ("other", UserSettingBuilder::fromValue (7.0f));
        effect->passOverrides.push_back (std::move (override));
    }
    effects.push_back (std::move (effect));
    // A script result of 2.5 reaches every bloom pass without truncation.
    applyBloomConstants (effects, 2.5f, 0.4f, glm::vec3 (1.0f));
    for (const auto& pass : effects.front ()->passOverrides) {
        REQUIRE (pass->constants.at ("bloomstrength")->value->getFloat () == 2.5f);
        REQUIRE (pass->constants.at ("bloomthreshold")->value->getFloat () == 0.4f);
        REQUIRE (pass->constants.at ("other")->value->getFloat () == 7.0f);
    }
}

TEST_CASE ("Non-HDR bloom passes carry the live general bloom tint", "[script][scene][bloom]") {
    using WallpaperEngine::Data::Model::ImageEffect;
    using WallpaperEngine::Data::Model::ImageEffectPassOverride;
    using WallpaperEngine::Data::Model::ImageEffectUniquePtr;
    using WallpaperEngine::Render::Wallpapers::applyBloomConstants;
    // Native's non-HDR branch uploads bloomtint beside
    // strength and threshold; shipped downsample_quarter_bloom.frag multiplies
    // the bloom colour by g_BloomTint ("material":"bloomtint"). Installed
    // 3222648892 binds it to its scheme colour (0.7529 grey).
    std::vector<ImageEffectUniquePtr> effects;
    auto effect = std::make_unique<ImageEffect> ();
    auto override = std::make_unique<ImageEffectPassOverride> ();
    override->constants.emplace ("bloomstrength", UserSettingBuilder::fromValue (0.93f));
    override->constants.emplace ("bloomthreshold", UserSettingBuilder::fromValue (0.65f));
    override->constants.emplace ("bloomtint", UserSettingBuilder::fromValue (glm::vec3 (1.0f)));
    auto* tint = override->constants.at ("bloomtint")->value.get ();
    // CPass binds the uniform to this storage once; later updates must land in it.
    const glm::vec3* bound = &tint->getVec3 ();
    effect->passOverrides.push_back (std::move (override));
    effects.push_back (std::move (effect));
    const glm::vec3 scheme (0.7529412f);
    applyBloomConstants (effects, 0.93f, 0.65f, scheme);
    REQUIRE (*bound == scheme);
    applyBloomConstants (effects, 0.93f, 0.65f, glm::vec3 (0.2f, 0.4f, 0.6f));
    REQUIRE (*bound == glm::vec3 (0.2f, 0.4f, 0.6f));
}

