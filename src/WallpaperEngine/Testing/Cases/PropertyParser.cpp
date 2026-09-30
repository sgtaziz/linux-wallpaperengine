#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Builders/ColorBuilder.h"
#include "WallpaperEngine/Data/Parsers/PropertyParser.h"
#include "WallpaperEngine/Data/Parsers/DynamicValueParser.h"
#include <quickjs.h>
#include "WallpaperEngine/Data/Parsers/UserSettingParser.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"

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
