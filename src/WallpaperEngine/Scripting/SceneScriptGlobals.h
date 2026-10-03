#pragma once

#include "quickjs.h"

namespace WallpaperEngine::Scripting {
/**
 * Defines a SceneScript global or interface member as an ordinary data
 * property: writable, enumerable and configurable. Native
 * installs console, engine, input, localStorage and thisScene with
 * v8::Object::Set and the console log/error members with template
 * attribute None, and the shipped typings
 * declare them with `let`. Scripts may therefore reassign them, for example
 * obfuscated code that runs `that.console = that.console || {}` and then
 * replaces each console method. `shared` (also `declare let`) is reassigned
 * at top level by installed scripts, which native 2.8.42 runs without error.
 */
inline int defineSceneScriptValue (JSContext* context, JSValueConst object, const char* name, JSValue value) {
    return JS_DefinePropertyValueStr (context, object, name, value, JS_PROP_C_W_E);
}
}
