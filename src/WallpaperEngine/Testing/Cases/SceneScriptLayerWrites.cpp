#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Data/Builders/UserSettingBuilder.h"
#include "WallpaperEngine/Scripting/LayerPropertyWrites.h"
#include "WallpaperEngine/Scripting/SceneSettingProperties.h"

#include <cmath>
#include <limits>
#include <string>

using WallpaperEngine::Scripting::convertLayerBoolean;
using WallpaperEngine::Scripting::convertLayerFloat;
using WallpaperEngine::Scripting::convertLayerInt;
using WallpaperEngine::Scripting::convertLayerString;
using WallpaperEngine::Scripting::convertLayerVector;
using WallpaperEngine::Scripting::defineLayerExpando;
using WallpaperEngine::Scripting::layerPropertyKey;
using WallpaperEngine::Scripting::LayerWrite;

namespace {
struct Context {
    JSRuntime* runtime = JS_NewRuntime ();
    JSContext* context = JS_NewContext (runtime);
    ~Context () {
        JS_FreeContext (context);
        JS_FreeRuntime (runtime);
    }

    JSValue eval (const char* source) {
        return JS_Eval (context, source, std::char_traits<char>::length (source), "<value>", JS_EVAL_TYPE_GLOBAL);
    }

    std::string evaluateModule (const char* source) {
        JSValue result = JS_Eval (context, source, std::char_traits<char>::length (source), "<module>",
                                  JS_EVAL_TYPE_MODULE);
        std::string error;
        if (JS_IsException (result) || JS_PromiseState (context, result) == JS_PROMISE_REJECTED) {
            JSValue exception = JS_IsException (result) ? JS_GetException (context) : JS_PromiseResult (context, result);
            const char* text = JS_ToCString (context, exception);
            error = text ? text : "exception";
            JS_FreeCString (context, text);
            JS_FreeValue (context, exception);
        }
        JS_FreeValue (context, result);
        return error;
    }
};

// A layer wrapper with a String and a Float host property. It routes writes
// the way the scriptable-object adapter does: host names go through their
// converter; every other name is an expando.
std::string layerText = "initial";
float layerVolume = 0.5f;
JSClassID layerClass = 0;

JSValue layerGet (JSContext* context, JSValueConst, JSAtom atom, JSValueConst) {
    const char* name = JS_AtomToCString (context, atom);
    const std::string field = name ? name : "";
    JS_FreeCString (context, name);
    if (field == "volume") return JS_NewFloat64 (context, layerVolume);
    return field == "text" ? JS_NewStringLen (context, layerText.data (), layerText.size ()) : JS_UNDEFINED;
}

int layerSet (JSContext* context, JSValueConst, JSAtom atom, JSValueConst value, JSValueConst receiver, int flags) {
    const char* name = JS_AtomToCString (context, atom);
    const std::string field = name ? name : "";
    JS_FreeCString (context, name);
    if (field == "volume") {
        float converted = 0.0f;
        const auto result = convertLayerFloat (context, value, converted);
        if (result == LayerWrite::Assign) layerVolume = converted;
        return result == LayerWrite::Exception ? -1 : 1;
    }
    if (field != "text") return defineLayerExpando (context, receiver, atom, value, flags);
    std::string converted;
    switch (convertLayerString (context, value, converted)) {
        case LayerWrite::Exception: return -1;
        case LayerWrite::Ignore: return 1;
        case LayerWrite::Assign: layerText = converted; return 1;
    }
    return -1;
}

JSClassExoticMethods layerMethods {.get_property = layerGet, .set_property = layerSet};

void installLayer (Context& js) {
    JS_NewClassID (js.runtime, &layerClass);
    JSClassDef definition {.class_name = "Layer", .exotic = &layerMethods};
    REQUIRE (JS_NewClass (js.runtime, layerClass, &definition) == 0);
    JSValue global = JS_GetGlobalObject (js.context);
    JS_SetPropertyStr (js.context, global, "layer", JS_NewObjectClass (js.context, static_cast<int> (layerClass)));
    JS_FreeValue (js.context, global);
}
}

TEST_CASE ("String layer properties take ToString of non-string values", "[script][layer]") {
    Context js;
    const struct {
        const char* source;
        const char* expected;
    } cases[] {
        // Installed 3155776049 assigns `getLayer('Start Time').text = Date.now()`.
        {"1696339200123", "1696339200123"},
        {"0.1 + 0.2", "0.30000000000000004"},
        {"-0", "0"},
        {"true", "true"},
        {"[1, 2]", "1,2"},
        {"({toString () { return 'custom'; }})", "custom"},
        {"'kept'", "kept"},
    };
    for (const auto& item : cases) {
        JSValue value = js.eval (item.source);
        REQUIRE_FALSE (JS_IsException (value));
        std::string result = "unchanged";
        CHECK (convertLayerString (js.context, value, result) == LayerWrite::Assign);
        CHECK (result == item.expected);
        JS_FreeValue (js.context, value);
    }
}

TEST_CASE ("String layer properties skip null and undefined without an exception", "[script][layer]") {
    Context js;
    for (const char* source : {"null", "undefined"}) {
        JSValue value = js.eval (source);
        std::string result = "unchanged";
        CHECK (convertLayerString (js.context, value, result) == LayerWrite::Ignore);
        CHECK (result == "unchanged");
        CHECK_FALSE (JS_HasException (js.context));
    }
}

TEST_CASE ("A throwing ToString reaches the script", "[script][layer]") {
    Context js;
    JSValue value = js.eval ("Symbol ('s')");
    std::string result = "unchanged";
    CHECK (convertLayerString (js.context, value, result) == LayerWrite::Exception);
    CHECK (result == "unchanged");
    JSValue exception = JS_GetException (js.context);
    CHECK (JS_IsError (exception));
    JS_FreeValue (js.context, exception);
    JS_FreeValue (js.context, value);
}

TEST_CASE ("Unknown layer names are stored as expandos and the handler continues", "[script][layer]") {
    Context js;
    installLayer (js);
    layerText = "initial";
    // Installed 2955378002 updatePercentageLayer writes `opacity`, which text
    // layers do not have, and then sets the text.
    REQUIRE (js.evaluateModule ("layer.opacity = 1;\n"
                                "layer.text = (0.5 * 100).toFixed (1) + '%';\n"
                                "if (layer.opacity !== 1) throw new Error ('expando not readable');\n"
                                "layer.opacity = 0;\n"
                                "if (layer.opacity !== 0) throw new Error ('expando not updated');\n"
                                "if (!Object.keys (layer).includes ('opacity')) throw new Error ('not own');\n"
                                "layer.text = Date.now ();\n"
                                "const stamp = layer.text;\n"
                                "layer.text = undefined;\n"
                                "layer.text = null;\n"
                                "if (layer.text !== stamp) throw new Error ('null or undefined changed text');\n"
                                "if (!/^[0-9]+$/.test (stamp)) throw new Error ('not ToString');\n")
                 .empty ());
    CHECK (layerText.find_first_not_of ("0123456789") == std::string::npos);
}

TEST_CASE ("An expando write to a non-extensible layer throws in module code", "[script][layer]") {
    Context js;
    installLayer (js);
    CHECK (js.evaluateModule ("Object.preventExtensions (layer);\nlayer.opacity = 1;\n").find ("extensible")
           != std::string::npos);
}

TEST_CASE ("Boolean layer properties accept only booleans", "[script][layer]") {
    Context js;
    for (const char* source : {"true", "false"}) {
        JSValue value = js.eval (source);
        bool result = std::string (source) != "true";
        CHECK (convertLayerBoolean (value, result) == LayerWrite::Assign);
        CHECK (result == (std::string (source) == "true"));
    }
    // Native IsBoolean rejects numbers, strings and objects.
    for (const char* source : {"1", "0", "'true'", "null", "undefined", "({})", "new Boolean (true)"}) {
        JSValue value = js.eval (source);
        bool result = false;
        CHECK (convertLayerBoolean (value, result) == LayerWrite::Ignore);
        CHECK_FALSE (result);
        JS_FreeValue (js.context, value);
    }
}

TEST_CASE ("Int layer properties use ToInt32 for any number", "[script][layer]") {
    Context js;
    const struct {
        const char* source;
        int32_t expected;
    } cases[] {
        {"7", 7},
        {"3.7", 3},
        {"-3.7", -3},
        {"NaN", 0},
        {"Infinity", 0},
        {"-Infinity", 0},
        {"2 ** 32 + 5", 5},
        {"2 ** 31", std::numeric_limits<int32_t>::min ()},
        {"1e20", 1661992960},
    };
    for (const auto& item : cases) {
        JSValue value = js.eval (item.source);
        int32_t result = -1;
        CHECK (convertLayerInt (js.context, value, result) == LayerWrite::Assign);
        CHECK (result == item.expected);
    }
    for (const char* source : {"'5'", "true", "undefined", "null", "5n", "({valueOf () { return 5; }})"}) {
        JSValue value = js.eval (source);
        int32_t result = -1;
        CHECK (convertLayerInt (js.context, value, result) == LayerWrite::Ignore);
        CHECK (result == -1);
        JS_FreeValue (js.context, value);
    }
}

TEST_CASE ("Float layer properties keep NaN and Infinity and skip non-numbers", "[script][layer]") {
    Context js;
    const auto convert = [&] (const char* source, float& result) {
        JSValue value = js.eval (source);
        const auto written = convertLayerFloat (js.context, value, result);
        JS_FreeValue (js.context, value);
        return written;
    };
    float result = 0.0f;
    CHECK (convert ("0.25", result) == LayerWrite::Assign);
    CHECK (result == 0.25f);
    CHECK (convert ("NaN", result) == LayerWrite::Assign);
    CHECK (std::isnan (result));
    CHECK (convert ("-Infinity", result) == LayerWrite::Assign);
    CHECK (result == -std::numeric_limits<float>::infinity ());
    // cvtsd2ss rounds to nearest: 2^128 - 2^104 is the largest float, and
    // the halfway point 2^128 - 2^103 rounds up to Infinity.
    CHECK (convert ("2 ** 128 - 2 ** 103 - 2 ** 80", result) == LayerWrite::Assign);
    CHECK (result == std::numeric_limits<float>::max ());
    CHECK (convert ("-(2 ** 128 - 2 ** 103)", result) == LayerWrite::Assign);
    CHECK (result == -std::numeric_limits<float>::infinity ());
    CHECK (convert ("1e300", result) == LayerWrite::Assign);
    CHECK (result == std::numeric_limits<float>::infinity ());
    // Installed 2955378002 and 2896906752 write `array[i].volume = shared.volume`
    // before `shared.volume` is set.
    result = 0.5f;
    for (const char* source : {"undefined", "null", "'1'", "true", "[1]"}) {
        CHECK (convert (source, result) == LayerWrite::Ignore);
        CHECK (result == 0.5f);
    }
    CHECK_FALSE (JS_HasException (js.context));
}

TEST_CASE ("Vector layer properties read object components or broadcast a number", "[script][layer]") {
    Context js;
    const auto convert = [&] (const char* source, int count, bool angle, float* components) {
        JSValue value = js.eval (source);
        REQUIRE_FALSE (JS_IsException (value));
        const auto written = convertLayerVector (js.context, value, count, angle, components);
        JS_FreeValue (js.context, value);
        return written;
    };
    float vector[4] {-1.0f, -1.0f, -1.0f, -1.0f};
    CHECK (convert ("({x: 1, y: 2, z: 3, w: 4})", 3, false, vector) == LayerWrite::Assign);
    CHECK ((vector[0] == 1.0f && vector[1] == 2.0f && vector[2] == 3.0f && vector[3] == -1.0f));
    CHECK (convert ("2.5", 4, false, vector) == LayerWrite::Assign);
    CHECK ((vector[0] == 2.5f && vector[1] == 2.5f && vector[2] == 2.5f && vector[3] == 2.5f));
    CHECK (convert ("({x: NaN, y: Infinity})", 2, false, vector) == LayerWrite::Assign);
    CHECK ((std::isnan (vector[0]) && std::isinf (vector[1])));

    // A missing or non-number component skips the whole write.
    for (const char* source : {"({x: 1, y: 2})", "({x: 1, y: '2', z: 3})", "[1, 2, 3]", "'1 2 3'", "null",
                               "undefined", "true"}) {
        float unchanged[3] {7.0f, 7.0f, 7.0f};
        CHECK (convert (source, 3, false, unchanged) == LayerWrite::Ignore);
        CHECK ((unchanged[0] == 7.0f && unchanged[1] == 7.0f && unchanged[2] == 7.0f));
    }
    CHECK_FALSE (JS_HasException (js.context));
}

TEST_CASE ("Angle objects convert from degrees but a broadcast number does not", "[script][layer]") {
    Context js;
    float angles[3] {};
    JSValue object = js.eval ("({x: 90, y: 0, z: -180})");
    CHECK (convertLayerVector (js.context, object, 3, true, angles) == LayerWrite::Assign);
    JS_FreeValue (js.context, object);
    CHECK (angles[0] == 90.0f * 0.0174532924f);
    CHECK (angles[1] == 0.0f);
    CHECK (angles[2] == -180.0f * 0.0174532924f);
    // The scalar path stores the number on every axis unchanged.
    JSValue scalar = js.eval ("90");
    CHECK (convertLayerVector (js.context, scalar, 3, true, angles) == LayerWrite::Assign);
    CHECK ((angles[0] == 90.0f && angles[1] == 90.0f && angles[2] == 90.0f));
}

TEST_CASE ("Vector components are all read before any is checked", "[script][layer]") {
    Context js;
    JSValue value = js.eval ("globalThis.reads = [];\n"
                             "({get x () { reads.push ('x'); return 'bad'; },\n"
                             "  get y () { reads.push ('y'); return 2; },\n"
                             "  get z () { reads.push ('z'); return 3; }})");
    float vector[3] {7.0f, 7.0f, 7.0f};
    CHECK (convertLayerVector (js.context, value, 3, false, vector) == LayerWrite::Ignore);
    JS_FreeValue (js.context, value);
    CHECK (vector[0] == 7.0f);
    JSValue reads = js.eval ("reads.join ('')");
    const char* order = JS_ToCString (js.context, reads);
    CHECK (std::string (order) == "xyz");
    JS_FreeCString (js.context, order);
    JS_FreeValue (js.context, reads);

    JSValue throwing = js.eval ("({get x () { throw new Error ('component'); }})");
    CHECK (convertLayerVector (js.context, throwing, 2, false, vector) == LayerWrite::Exception);
    JS_FreeValue (js.context, throwing);
    JSValue exception = JS_GetException (js.context);
    CHECK (JS_IsError (exception));
    JS_FreeValue (js.context, exception);
}

TEST_CASE ("A wrong-typed write leaves the layer unchanged and the handler continues", "[script][layer]") {
    Context js;
    installLayer (js);
    layerText = "initial";
    layerVolume = 0.5f;
    REQUIRE (js.evaluateModule ("const shared = {};\n"
                                "layer.volume = shared.volume;\n"
                                "layer.volume = 'loud';\n"
                                "if (layer.volume !== 0.5) throw new Error ('volume changed');\n"
                                "layer.text = 'after';\n"
                                "layer.volume = NaN;\n")
                 .empty ());
    CHECK (layerText == "after");
    CHECK (std::isnan (layerVolume));
}

TEST_CASE ("Layer property names match native host names exactly", "[script][layer]") {
    // Text settings are registered in lowercase and looked up
    // case-sensitively.
    CHECK (layerPropertyKey ("backgroundbrightness") == std::optional<std::string_view> ("backgroundBrightness"));
    CHECK (layerPropertyKey ("pointsize") == std::optional<std::string_view> ("pointSize"));
    CHECK (layerPropertyKey ("backgroundcolor") == std::optional<std::string_view> ("backgroundColor"));
    // The renderer's camel-case keys are not native names.
    CHECK_FALSE (layerPropertyKey ("backgroundBrightness").has_value ());
    CHECK_FALSE (layerPropertyKey ("pointSize").has_value ());
    CHECK_FALSE (layerPropertyKey ("maxRows").has_value ());
    // Other names pass through unchanged, including native camel-case ones.
    CHECK (layerPropertyKey ("parallaxDepth") == std::optional<std::string_view> ("parallaxDepth"));
    CHECK (layerPropertyKey ("Origin") == std::optional<std::string_view> ("Origin"));
}

TEST_CASE ("thisScene general properties map to their own scene fields with native host types",
           "[script][scene]") {
    using WallpaperEngine::Data::Builders::UserSettingBuilder;
    using WallpaperEngine::Data::Model::SceneData;
    using WallpaperEngine::Scripting::SceneSettingProperty;
    using WallpaperEngine::Scripting::SceneSettingType;
    using WallpaperEngine::Scripting::sceneSettingProperties;
    SceneData scene {};
    scene.clearEnabled = UserSettingBuilder::fromValue (true);
    scene.colors.ambient = UserSettingBuilder::fromValue (glm::vec3 (0.1f));
    scene.colors.skylight = UserSettingBuilder::fromValue (glm::vec3 (0.2f));
    scene.colors.clear = UserSettingBuilder::fromValue (glm::vec3 (0.3f));
    scene.camera.bloom.enabled = UserSettingBuilder::fromValue (false);
    scene.camera.bloom.strength = UserSettingBuilder::fromValue (2.5f);
    const auto find = [] (const char* name) -> const SceneSettingProperty* {
        for (const auto& property : sceneSettingProperties ())
            if (std::string (property.name) == name) return &property;
        return nullptr;
    };
    // clearenabled is the clear flag (it used to read bloom), and
    // skylightcolor is not the ambient color.
    REQUIRE (&find ("clearenabled")->setting (scene) == &scene.clearEnabled);
    REQUIRE (find ("clearenabled")->setting (scene)->value->getBool ());
    REQUIRE (&find ("bloom")->setting (scene) == &scene.camera.bloom.enabled);
    REQUIRE (&find ("skylightcolor")->setting (scene) == &scene.colors.skylight);
    REQUIRE (&find ("ambientcolor")->setting (scene) == &scene.colors.ambient);
    REQUIRE (&find ("bloomstrength")->setting (scene) == &scene.camera.bloom.strength);
    // Native host types: 6 bool, 4 float, 2 Vec3.
    for (const char* name : {"bloom", "clearenabled", "camerafade", "camerashake", "cameraparallax"})
        REQUIRE (find (name)->type == SceneSettingType::Bool);
    for (const char* name : {"bloomstrength", "bloomthreshold", "fov", "nearz", "farz", "camerashakespeed",
                             "camerashakeamplitude", "camerashakeroughness", "cameraparallaxamount",
                             "cameraparallaxdelay", "cameraparallaxmouseinfluence"})
        REQUIRE (find (name)->type == SceneSettingType::Float);
    for (const char* name : {"clearcolor", "ambientcolor", "skylightcolor"})
        REQUIRE (find (name)->type == SceneSettingType::Vec3);
}

TEST_CASE ("thisScene setters store converted values and reads keep float precision", "[script][scene]") {
    using WallpaperEngine::Data::Builders::UserSettingBuilder;
    using WallpaperEngine::Data::Model::DynamicValue;
    using WallpaperEngine::Scripting::SceneSettingType;
    using WallpaperEngine::Scripting::sceneSettingScalarValue;
    using WallpaperEngine::Scripting::storeSceneSetting;
    Context js;
    const auto store = [&] (const char* source, DynamicValue& value, SceneSettingType type) {
        JSValue input = js.eval (source);
        REQUIRE_FALSE (JS_IsException (input));
        const bool stored = storeSceneSetting (js.context, input, value, type);
        JS_FreeValue (js.context, input);
        return stored;
    };
    const auto read = [&] (const DynamicValue& value, SceneSettingType type) {
        JSValue result = sceneSettingScalarValue (js.context, value, type);
        double number = -1.0;
        if (JS_IsBool (result)) number = JS_VALUE_GET_BOOL (result) ? 1.0 : 0.0;
        else REQUIRE (JS_ToFloat64 (js.context, &number, result) == 0);
        JS_FreeValue (js.context, result);
        return number;
    };

    // Native type 4 is a float: a strength of 2.5 must not
    // read back as the integer 2.
    auto strength = UserSettingBuilder::fromValue (1.0f);
    REQUIRE (store ("2.5", *strength->value, SceneSettingType::Float));
    REQUIRE (strength->value->getFloat () == 2.5f);
    REQUIRE (read (*strength->value, SceneSettingType::Float) == 2.5);
    // A value of the wrong type is skipped without an exception.
    REQUIRE (store ("'3'", *strength->value, SceneSettingType::Float));
    REQUIRE (strength->value->getFloat () == 2.5f);
    REQUIRE_FALSE (JS_HasException (js.context));

    auto enabled = UserSettingBuilder::fromValue (true);
    REQUIRE (store ("false", *enabled->value, SceneSettingType::Bool));
    REQUIRE_FALSE (enabled->value->getBool ());
    REQUIRE (store ("1", *enabled->value, SceneSettingType::Bool));
    REQUIRE_FALSE (enabled->value->getBool ());
    REQUIRE (read (*enabled->value, SceneSettingType::Bool) == 0.0);

    auto color = UserSettingBuilder::fromValue (glm::vec3 (0.0f));
    REQUIRE (store ("({x:0.25,y:0.5,z:0.75})", *color->value, SceneSettingType::Vec3));
    REQUIRE (color->value->getVec3 () == glm::vec3 (0.25f, 0.5f, 0.75f));
    REQUIRE (store ("({x:1,y:1})", *color->value, SceneSettingType::Vec3));
    REQUIRE (color->value->getVec3 () == glm::vec3 (0.25f, 0.5f, 0.75f));
    REQUIRE (store ("0.5", *color->value, SceneSettingType::Vec3));
    REQUIRE (color->value->getVec3 () == glm::vec3 (0.5f));

    // A throwing component read is the only failure; the exception stays
    // pending for the setter to propagate.
    REQUIRE_FALSE (store ("({get x(){ throw new Error('boom'); },y:0,z:0})", *color->value,
                          SceneSettingType::Vec3));
    REQUIRE (JS_HasException (js.context));
    JS_FreeValue (js.context, JS_GetException (js.context));
    REQUIRE (color->value->getVec3 () == glm::vec3 (0.5f));
}
