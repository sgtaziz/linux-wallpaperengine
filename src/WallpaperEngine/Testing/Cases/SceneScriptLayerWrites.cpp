#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Scripting/LayerPropertyWrites.h"

#include <string>

using WallpaperEngine::Scripting::convertLayerString;
using WallpaperEngine::Scripting::defineLayerExpando;
using WallpaperEngine::Scripting::LayerStringWrite;

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

// A layer wrapper with one String host property. It routes writes the way
// the scriptable-object adapter does: the host name converts as String;
// every other name is an expando.
std::string layerText = "initial";
JSClassID layerClass = 0;

JSValue layerGet (JSContext* context, JSValueConst, JSAtom atom, JSValueConst) {
    const char* name = JS_AtomToCString (context, atom);
    const bool text = name && std::string (name) == "text";
    JS_FreeCString (context, name);
    return text ? JS_NewStringLen (context, layerText.data (), layerText.size ()) : JS_UNDEFINED;
}

int layerSet (JSContext* context, JSValueConst, JSAtom atom, JSValueConst value, JSValueConst receiver, int flags) {
    const char* name = JS_AtomToCString (context, atom);
    const bool text = name && std::string (name) == "text";
    JS_FreeCString (context, name);
    if (!text) return defineLayerExpando (context, receiver, atom, value, flags);
    std::string converted;
    switch (convertLayerString (context, value, converted)) {
        case LayerStringWrite::Exception: return -1;
        case LayerStringWrite::Ignore: return 1;
        case LayerStringWrite::Assign: layerText = converted; return 1;
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
        CHECK (convertLayerString (js.context, value, result) == LayerStringWrite::Assign);
        CHECK (result == item.expected);
        JS_FreeValue (js.context, value);
    }
}

TEST_CASE ("String layer properties skip null and undefined without an exception", "[script][layer]") {
    Context js;
    for (const char* source : {"null", "undefined"}) {
        JSValue value = js.eval (source);
        std::string result = "unchanged";
        CHECK (convertLayerString (js.context, value, result) == LayerStringWrite::Ignore);
        CHECK (result == "unchanged");
        CHECK_FALSE (JS_HasException (js.context));
    }
}

TEST_CASE ("A throwing ToString reaches the script", "[script][layer]") {
    Context js;
    JSValue value = js.eval ("Symbol ('s')");
    std::string result = "unchanged";
    CHECK (convertLayerString (js.context, value, result) == LayerStringWrite::Exception);
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
