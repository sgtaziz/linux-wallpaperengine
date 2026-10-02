#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Builders/ColorBuilder.h"
#include "WallpaperEngine/Data/Parsers/PropertyParser.h"
#include "WallpaperEngine/Data/Parsers/DynamicValueParser.h"
#include <quickjs.h>
#include "WallpaperEngine/Data/Parsers/UserSettingParser.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/ProjectParser.h"
#include "WallpaperEngine/WebBrowser/WebPropertyPayload.h"
#include "WallpaperEngine/WebBrowser/WebAudioFrame.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include <fstream>
#include <chrono>

using WallpaperEngine::Data::JSON::JSON;
using WallpaperEngine::Data::Model::DynamicValue;
using WallpaperEngine::Data::Parsers::PropertyParser;

TEST_CASE ("Bool properties without a value default to false") {
    const JSON propertyData = {
	{ "type", "bool" },
	{ "text", "Enabled" },
    };

    const auto property = PropertyParser::parse (propertyData, "enabled");

    REQUIRE (property != nullptr);
    CHECK (property->getType () == DynamicValue::Boolean);
    CHECK_FALSE (property->getBool ());
}

TEST_CASE ("Directory properties are parsed as file-like properties") {
    const JSON propertyData = {
	{ "type", "directory" },
	{ "text", "Folder" },
    };

    const auto property = PropertyParser::parse (propertyData, "folder");

    REQUIRE (property != nullptr);
    CHECK (property->dump ().find ("folder - file") != std::string::npos);
}

TEST_CASE ("Project color properties use normalized channels even without decimals") {
    const auto parse = [] (const std::string& value) {
        return PropertyParser::parse (JSON {{"type", "color"}, {"value", value}}, "tint");
    };
    CHECK (parse ("1 1 1")->getVec4 () == glm::vec4 (1.0f));
    CHECK (parse ("1 0 0")->getVec4 () == glm::vec4 (1.0f, 0.0f, 0.0f, 1.0f));
    CHECK (parse ("0.5 0.25 1")->getVec4 () == glm::vec4 (0.5f, 0.25f, 1.0f, 1.0f));
    CHECK (parse ("0 0 1 0.5")->getVec4 () == glm::vec4 (0.0f, 0.0f, 1.0f, 0.5f));
    auto live = parse ("0 0 0");
    live->update (std::string ("1 0 0"), DynamicValue::UpdateSource::User);
    CHECK (live->getVec4 () == glm::vec4 (1.0f, 0.0f, 0.0f, 1.0f));
    // Byte-oriented color consumers retain their separate interpretation.
    CHECK (WallpaperEngine::Data::Builders::ColorBuilder::parse ("255 0 0").r == 1.0f);
    CHECK (WallpaperEngine::Data::Builders::ColorBuilder::parse ("1 0 0").r == 1.0f / 255.0f);
}

TEST_CASE ("Authored scene color follows normalized project property") {
    auto color = PropertyParser::parse (JSON {{"type", "color"}, {"value", "1 1 1"}}, "clockcolor");
    WallpaperEngine::Data::Model::Properties properties {{"clockcolor", color}};
    auto setting = WallpaperEngine::Data::Parsers::UserSettingParser::parse (
        JSON {{"user", "clockcolor"}, {"value", "255 255 255"}}, properties, true);

    REQUIRE (setting != nullptr);
    CHECK (setting->value->getVec4 () == glm::vec4 (1.0f));
    color->update (std::string ("1 0 0"), DynamicValue::UpdateSource::User);
    CHECK (setting->value->getVec4 () == glm::vec4 (1.0f, 0.0f, 0.0f, 1.0f));
}

TEST_CASE ("String combo options retain their JavaScript type") {
    const std::string script =
        "var scriptProperties = createScriptProperties()"
        ".addCombo({ name: 'direction', options: [{ value: '0' }, { value: '1' }] })"
        ".addCombo({ name: 'numeric', options: [{ value: 0 }, { value: 1 }] })"
        ".addSlider({ name: 'speed', value: 5.6 }).finish();"
        "function update(value) { switch(scriptProperties.direction) {"
        "case('1'): value.x -= scriptProperties.speed; break; } return value; }";
    const JSON source = {
        {"value", "5200 2144 0"},
        {"script", script},
        {"scriptproperties", {{"direction", "1"}, {"numeric", "1"}, {"speed", "5.6"}}},
    };
    const auto parsed = WallpaperEngine::Data::Parsers::DynamicValueParser::parse (source, {}, false);
    const auto& properties = parsed->getProperties ();
    CHECK (properties.at ("direction")->value->getType () == DynamicValue::String);
    CHECK (properties.at ("direction")->value->getString () == "1");
    CHECK (properties.at ("numeric")->value->getType () == DynamicValue::Float);
    CHECK (properties.at ("speed")->value->getType () == DynamicValue::Float);

    // Exercise the same strict switch and origin update used by the shipped
    // scrolling text: a numeric 1 silently skips the string case and leaves
    // the layer at its offscreen X=5200 starting position.
    JSRuntime* runtime = JS_NewRuntime ();
    REQUIRE (runtime != nullptr);
    JSContext* context = JS_NewContext (runtime);
    REQUIRE (context != nullptr);
    JSValue global = JS_GetGlobalObject (context);
    JSValue seed = JS_NewObject (context);
    JS_SetPropertyStr (context, seed, "direction", JS_NewString (context,
        properties.at ("direction")->value->getString ().c_str ()));
    JS_SetPropertyStr (context, seed, "speed", JS_NewFloat64 (context,
        properties.at ("speed")->value->getFloat ()));
    JS_SetPropertyStr (context, global, "seed", seed);
    JS_FreeValue (context, global);
    const std::string evaluation =
        "function createScriptProperties() { return {"
        "addCombo() { return this; }, addSlider() { return this; },"
        "finish() { return seed; }}; }" + script +
        "var origin = {x:5200,y:2144,z:0}; update(origin); update(origin); origin.x;";
    JSValue result = JS_Eval (context, evaluation.c_str (), evaluation.size (),
                              "<scrolling-origin-test>", JS_EVAL_TYPE_GLOBAL);
    REQUIRE_FALSE (JS_IsException (result));
    double x = 0.0;
    REQUIRE (JS_ToFloat64 (context, &x, result) == 0);
    CHECK (x == Catch::Approx (5188.8));
    JS_FreeValue (context, result);
    JS_FreeContext (context);
    JS_FreeRuntime (runtime);
}

TEST_CASE ("Scene RGB strings retain floating channels and live property updates", "[scene][color][parser]") {
    const auto color = [] (const std::string& value) {
        const JSON data {{"color", {{"value", value}, {"script", "export function init(v) { return v; }"}}}};
        return data.color ("color", {}, true);
    };
    CHECK (color ("1 1 1")->value->getVec4 () == glm::vec4 (1.0f));
    CHECK (color ("1.0 1.0 1.0")->value->getVec4 () == glm::vec4 (1.0f));
    CHECK (color ("255 128 64")->value->getVec4 () == glm::vec4 (255.0f, 128.0f, 64.0f, 1.0f));
    CHECK (color ("0.5 0.25 0.125")->value->getVec4 () == glm::vec4 (0.5f, 0.25f, 0.125f, 1.0f));
    REQUIRE (color ("1 1 1")->value->getScriptSource ().has_value ());
    WallpaperEngine::Data::Model::Project project {};
    const auto text = WallpaperEngine::Data::Parsers::ObjectParser::parse (
        JSON::parse (R"({"id":12,"name":"RGB text","text":"A","color":"255 128 64"})"), project);
    REQUIRE (text->is<WallpaperEngine::Data::Model::Text> ());
    CHECK (text->as<WallpaperEngine::Data::Model::Text> ()->color->value->getVec4 ()
           == glm::vec4 (255.0f, 128.0f, 64.0f, 1.0f));
    auto property = PropertyParser::parse (JSON {{"type", "color"}, {"value", "1 1 1"}}, "tint");
    WallpaperEngine::Data::Model::Properties properties {{"tint", property}};
    const JSON data {{"color", {{"value", "255 128 64"}, {"user", "tint"}}}};
    auto setting = data.color ("color", properties, true);
    CHECK (setting->value->getVec4 () == glm::vec4 (1.0f));
    property->update (std::string ("0.25 0.5 1"), DynamicValue::UpdateSource::User);
    CHECK (setting->value->getVec4 () == glm::vec4 (0.25f, 0.5f, 1.0f, 1.0f));
}

namespace {
struct PresetProjects {
    std::filesystem::path root = std::filesystem::temp_directory_path ()
        / ("wallpaper-presets-" + std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ()));
    PresetProjects () { std::filesystem::create_directories (root); }
    ~PresetProjects () { std::filesystem::remove_all (root); }
    void write (const std::string& path, const std::string& value) {
        const auto file = root / path;
        std::filesystem::create_directories (file.parent_path ());
        std::ofstream (file) << value;
    }
    void package (const std::string& path, const std::string& metadata) {
        const auto file = root / path;
        std::filesystem::create_directories (file.parent_path ());
        std::ofstream output (file, std::ios::binary);
        const auto uint32 = [&] (uint32_t value) {
            for (unsigned int shift = 0; shift < 32; shift += 8) output.put ((value >> shift) & 0xff);
        };
        const auto string = [&] (const std::string& value) { uint32 (value.size ()); output << value; };
        string ("PKGV0001");
        uint32 (1);
        string ("project.json");
        uint32 (0);
        uint32 (metadata.size ());
        output << metadata;
    }
};
}

TEST_CASE ("Dependent presets inherit a renderer and preserve nested values and asset precedence", "[project-presets]") {
    using WallpaperEngine::Data::Parsers::ProjectParser;
    PresetProjects files;
    files.write ("100/project.json", R"({"type":"web","title":"base","file":"index.html","workshopid":100,"general":{"supportsaudioprocessing":true,"properties":{"image":{"type":"file","value":""},"width":{"type":"slider","value":3},"direction":{"type":"combo","value":1,"options":[{"value":1,"label":"one"},{"value":2,"label":"two"}]},"textmode":{"type":"combo","value":"1","options":[{"value":"1","label":"one"}]},"enabled":{"type":"bool","value":true},"tint":{"type":"color","value":"1 0 0"}}}})");
    files.write ("100/index.html", "base entry");
    files.write ("100/shared.txt", "base asset");
    files.write ("200/project.json", R"({"title":"middle","dependency":"100","file":"","preset":{"width":4,"enabled":false}})");
    files.write ("300/project.json", R"({"title":"selected","type":null,"file":null,"general":{"supportsaudioprocessing":false,"properties":{"width":{"type":"slider","value":55}}},"dependency":"200","preset":{"image":"files/picture.png","width":7,"direction":2,"ignored":"unused","enabled":null}})");
    files.write ("300/files/picture.png", "selected image");
    files.write ("300/shared.txt", "child asset");
    const auto resolved = ProjectParser::resolve (files.root / "300");
    REQUIRE (resolved.assetDirectories.size () == 3);
    CHECK (resolved.data.at ("type") == "web");
    CHECK (resolved.data.at ("file") == "index.html");
    CHECK (resolved.data.at ("title") == "selected");
    CHECK_FALSE (resolved.data.contains ("workshopid"));
    CHECK_FALSE (resolved.data["general"]["properties"].contains ("ignored"));
    auto container = std::make_unique<WallpaperEngine::FileSystem::Container> ();
    for (const auto& path : resolved.assetDirectories) container->mount (path, "/");
    auto locator = std::make_unique<WallpaperEngine::Assets::AssetLocator> (std::move (container));
    CHECK (locator->readString ("index.html") == "base entry");
    CHECK (locator->readString ("shared.txt") == "child asset");
    auto project = ProjectParser::parse (resolved.data, std::move (locator));
    CHECK (project->type == WallpaperEngine::Data::Model::Project::Type_Web);
    CHECK (project->supportsAudioProcessing);
    CHECK (project->properties.at ("width")->getFloat () == 7);
    CHECK_FALSE (project->properties.at ("enabled")->getBool ());
    // CLI property updates occur before browser creation and must win over the preset.
    project->properties.at ("width")->update (std::string ("9"), DynamicValue::UpdateSource::User);
    const auto payload = nlohmann::json::parse (WallpaperEngine::WebBrowser::webPropertyPayload (*project));
    CHECK (payload["width"]["value"] == 9);
    CHECK (payload["direction"]["value"] == 2);
    CHECK (payload["textmode"]["value"] == "1");
    CHECK (payload["enabled"]["value"] == false);
    CHECK (payload["tint"]["value"] == "1 0 0");
    CHECK (payload["image"]["value"] == (files.root / "300/files/picture.png").string ());
    CHECK (WallpaperEngine::WebBrowser::webFilePropertyPaths (*project, payload.dump ())
        == std::set<std::filesystem::path> {files.root / "300/files/picture.png"});
    project->properties.at ("image")->update (std::string ("missing-external-image.png"), DynamicValue::UpdateSource::User);
    CHECK (nlohmann::json::parse (WallpaperEngine::WebBrowser::webPropertyPayload (*project))["image"]["value"]
        == "missing-external-image.png");
    CHECK (WallpaperEngine::WebBrowser::webFilePropertyPaths (*project,
        WallpaperEngine::WebBrowser::webPropertyPayload (*project)).empty ());
}

TEST_CASE ("Project dependencies reject missing cyclic and escaping IDs without guessing a scene", "[project-presets]") {
    using WallpaperEngine::Data::Parsers::ProjectParser;
    PresetProjects files;
    files.write ("100/project.json", R"({"title":"missing","dependency":"999","preset":{}})");
    REQUIRE_THROWS_WITH (ProjectParser::resolve (files.root / "100"),
        "Missing wallpaper dependency 999 required by " + (files.root / "100").string ()
        + "; install that Workshop wallpaper first");
    files.write ("100/project.json", R"({"dependency":"200","preset":{}})");
    files.write ("200/project.json", R"({"dependency":"100","preset":{}})");
    REQUIRE_THROWS_WITH (ProjectParser::resolve (files.root / "100"),
        "Project dependency cycle at " + (files.root / "100").string ());
    files.write ("100/project.json", R"({"dependency":"../200","preset":{}})");
    REQUIRE_THROWS_WITH (ProjectParser::resolve (files.root / "100"), "Invalid project dependency Workshop ID: ../200");
    // A complete project is not a preset just because it carries dependency metadata.
    files.write ("100/project.json", R"({"title":"direct","type":"web","file":"own.html","dependency":"999"})");
    CHECK (ProjectParser::resolve (files.root / "100").assetDirectories.size () == 1);
    // A registered library can supply an absent sibling without changing child precedence.
    files.write ("100/project.json", R"({"dependency":"200","preset":{}})");
    files.write ("remote/200/project.json", R"({"title":"remote","type":"web","file":"index.html"})");
    std::filesystem::remove_all (files.root / "200");
    const auto remote = ProjectParser::resolve (files.root / "100", [&] (const std::string& id) {
        return files.root / "remote" / id;
    });
    CHECK (remote.assetDirectories.at (0) == files.root / "100");
    CHECK (remote.assetDirectories.at (1) == files.root / "remote/200");
}

TEST_CASE ("Project metadata retains package fallback and direct projects ignore presets", "[project-presets]") {
    using WallpaperEngine::Data::Parsers::ProjectParser;
    PresetProjects files;
    files.package ("100/scene.pkg", R"({"title":"packaged","type":"web","file":"index.html","general":{"properties":{"image":{"type":"file","value":"base.png"},"enabled":{"type":"bool","value":true}}}})");
    CHECK (ProjectParser::resolve (files.root / "100").data.at ("title") == "packaged");
    files.write ("200/project.json", R"({"title":"selected","dependency":"100","preset":{"image":null,"enabled":null}})");
    const auto inherited = ProjectParser::resolve (files.root / "200");
    CHECK (inherited.data["general"]["properties"]["image"]["value"].is_null ());
    auto container = std::make_unique<WallpaperEngine::FileSystem::Container> ();
    auto project = ProjectParser::parse (inherited.data,
        std::make_unique<WallpaperEngine::Assets::AssetLocator> (std::move (container)));
    CHECK (project->properties.at ("image")->getString ().empty ());
    CHECK_FALSE (project->properties.at ("enabled")->getBool ());
    files.write ("100/project.json", R"({"title":"loose","type":"web","file":"index.html","general":{"properties":{"enabled":{"type":"bool","value":true}}},"preset":{"enabled":false},"dependency":"999"})");
    const auto direct = ProjectParser::resolve (files.root / "100");
    CHECK (direct.data.at ("title") == "loose");
    CHECK (direct.data["general"]["properties"]["enabled"]["value"] == true);
}

TEST_CASE ("Web audio frames preserve stereo separation and unbounded native amplitudes", "[project-presets][web-audio]") {
    float left[64] {};
    float right[64] {};
    CHECK (WallpaperEngine::WebBrowser::webAudioFrame (left, right) == std::array<double, 128> {});
    left[0] = 1.5f;
    left[63] = 0.25f;
    right[0] = 0.75f;
    right[63] = 2.0f;
    const auto samples = WallpaperEngine::WebBrowser::webAudioFrame (left, right);
    REQUIRE (samples.size () == 128);
    CHECK (samples[0] == 1.5);
    CHECK (samples[63] == 0.25);
    CHECK (samples[64] == 0.75);
    CHECK (samples[127] == 2.0);
    CHECK (samples[1] == 0);
    CHECK (samples[65] == 0);
}

TEST_CASE ("Web text input delivery preserves authored text and CLI replacement", "[project-presets]") {
    WallpaperEngine::Data::Model::Project project {};
    project.properties["label"] = PropertyParser::parse (
        JSON {{"type", "textinput"}, {"value", "text with \"quotes\""}}, "label");
    const auto authored = nlohmann::json::parse (WallpaperEngine::WebBrowser::webPropertyPayload (project));
    CHECK (authored["label"]["value"] == "text with \"quotes\"");
    project.properties["label"] = std::make_shared<WallpaperEngine::Data::Model::PropertyTextInput> (
        *std::dynamic_pointer_cast<WallpaperEngine::Data::Model::PropertyTextInput> (project.properties.at ("label")));
    CHECK (nlohmann::json::parse (WallpaperEngine::WebBrowser::webPropertyPayload (project))["label"]["value"]
        == "text with \"quotes\"");
    project.properties.at ("label")->update (std::string ("replacement text"), DynamicValue::UpdateSource::User);
    const auto edited = nlohmann::json::parse (WallpaperEngine::WebBrowser::webPropertyPayload (project));
    CHECK (edited["label"]["value"] == "replacement text");
    project.properties.at ("label")->update (std::string ("\"hello\""), DynamicValue::UpdateSource::User);
    const auto quoted = nlohmann::json::parse (WallpaperEngine::WebBrowser::webPropertyPayload (project));
    CHECK (quoted["label"]["value"] == "\"hello\"");
    const auto text = std::dynamic_pointer_cast<WallpaperEngine::Data::Model::PropertyTextInput> (
        project.properties.at ("label"));
    auto copy = std::make_shared<WallpaperEngine::Data::Model::PropertyTextInput> (*text);
    project.properties["label"] = copy;
    CHECK (nlohmann::json::parse (WallpaperEngine::WebBrowser::webPropertyPayload (project))["label"]["value"]
        == "\"hello\"");
    // An identical literal still changes provenance even though DynamicValue
    // avoids emitting a change notification for equal values.
    project.properties["label"] = PropertyParser::parse (JSON {{"type", "textinput"}, {"value", "hello"}}, "label");
    project.properties.at ("label")->update (std::string ("\"hello\""), DynamicValue::UpdateSource::User);
    CHECK (nlohmann::json::parse (WallpaperEngine::WebBrowser::webPropertyPayload (project))["label"]["value"]
        == "\"hello\"");
    project.properties.at ("label")->update (std::string ("\"script text\""), DynamicValue::UpdateSource::Script);
    CHECK (nlohmann::json::parse (WallpaperEngine::WebBrowser::webPropertyPayload (project))["label"]["value"]
        == "\"script text\"");
}
