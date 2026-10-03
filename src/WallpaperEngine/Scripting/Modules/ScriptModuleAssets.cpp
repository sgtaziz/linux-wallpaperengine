#include "ScriptModuleAssets.h"

#include <cctype>
#include <exception>
#include <utility>

using namespace WallpaperEngine::Scripting::Modules;

std::string WallpaperEngine::Scripting::Modules::scriptModuleAssetPath (std::string_view name) {
    std::string lowered (name);
    for (auto& character : lowered)
        character = static_cast<char> (std::tolower (static_cast<unsigned char> (character)));
    std::string path = "scripts/jsmodules/" + lowered;
    if (lowered.find (".js") == std::string::npos) path += ".js";
    return path;
}

char* WallpaperEngine::Scripting::Modules::normalizeScriptModuleName (
    JSContext* context, const char*, const char* name, void*) {
    char* normalized = js_strdup (context, name);
    if (!normalized) return nullptr;
    for (char* character = normalized; *character; ++character)
        *character = static_cast<char> (std::tolower (static_cast<unsigned char> (*character)));
    return normalized;
}

ScriptModuleAssets::ScriptModuleAssets (Reader reader) : m_reader (std::move (reader)) { }

JSModuleDef* ScriptModuleAssets::load (JSContext* context, const char* name) {
    const std::string path = scriptModuleAssetPath (name);
    if (const auto found = m_modules.find (path); found != m_modules.end ()) return found->second;

    std::string source;
    try {
        source = m_reader (path);
    } catch (const std::exception& error) {
        JS_ThrowReferenceError (context, "could not load module '%s' from %s: %s", name, path.c_str (),
                                error.what ());
        return nullptr;
    }

    // The import name is the module name, so QuickJS also finds a later
    // identical import among the context's loaded modules.
    JSValue compiled = JS_Eval (context, source.c_str (), source.size (), name,
                                JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException (compiled)) return nullptr;
    if (!JS_IsModule (compiled)) {
        JS_FreeValue (context, compiled);
        JS_ThrowSyntaxError (context, "%s is not a module", path.c_str ());
        return nullptr;
    }
    // The context retains compiled modules; release only this reference.
    auto* definition = static_cast<JSModuleDef*> (JS_VALUE_GET_PTR (compiled));
    JS_FreeValue (context, compiled);
    m_modules.emplace (path, definition);
    return definition;
}
