#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Scripting/SceneScriptGlobals.h"

#include <string>

using WallpaperEngine::Scripting::defineSceneScriptValue;

namespace {
JSValue noop (JSContext*, JSValueConst, int, JSValueConst*) { return JS_UNDEFINED; }

struct Context {
    JSRuntime* runtime = JS_NewRuntime ();
    JSContext* context = JS_NewContext (runtime);
    ~Context () {
        JS_FreeContext (context);
        JS_FreeRuntime (runtime);
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
}

TEST_CASE ("SceneScript globals accept the console-replacement idiom of obfuscated scripts", "[script][globals]") {
    Context js;
    JSValue global = JS_GetGlobalObject (js.context);
    JSValue console = JS_NewObject (js.context);
    REQUIRE (defineSceneScriptValue (js.context, console, "log", JS_NewCFunction (js.context, noop, "log", 1)) >= 0);
    REQUIRE (defineSceneScriptValue (js.context, console, "error", JS_NewCFunction (js.context, noop, "error", 1)) >= 0);
    REQUIRE (defineSceneScriptValue (js.context, global, "console", console) >= 0);
    for (const char* name : {"engine", "input", "localStorage", "thisScene", "shared"})
        REQUIRE (defineSceneScriptValue (js.context, global, name, JS_NewObject (js.context)) >= 0);
    JS_FreeValue (js.context, global);

    // The javascript-obfuscator "disable console output" prologue used by
    // installed 3257420414, run in strict module scope like an authored script.
    REQUIRE (js.evaluateModule (
                 "const that = Function('return (function() {}.constructor(\"return this\")( ));')();\n"
                 "const original = console.log;\n"
                 "const consoleObject = that.console = that.console || {};\n"
                 "for (const method of ['log', 'warn', 'info', 'error', 'exception', 'table', 'trace']) {\n"
                 "  const replacement = function () {}.bind();\n"
                 "  consoleObject[method] = replacement;\n"
                 "}\n"
                 "if (console !== consoleObject || console.log === original) throw new Error('not replaced');\n"
                 "engine = engine; input = input; localStorage = localStorage; thisScene = thisScene;\n"
                 // Installed 2938612768 /objects/78 replaces the shared object at top level;
                 // later reads from any module see the replacement.
                 "shared = {a: false, b: false, c: false};\n"
                 "if (globalThis.shared.a !== false) throw new Error('shared not replaced');\n")
             .empty ());
}

TEST_CASE ("A read-only SceneScript global rejects that idiom", "[script][globals]") {
    // Documents the Linux defect the writable definition removes.
    Context js;
    JSValue global = JS_GetGlobalObject (js.context);
    JS_DefinePropertyValueStr (js.context, global, "console", JS_NewObject (js.context), JS_PROP_ENUMERABLE);
    JS_FreeValue (js.context, global);
    REQUIRE (js.evaluateModule ("globalThis.console = globalThis.console || {};\n").find ("read-only")
             != std::string::npos);
}
