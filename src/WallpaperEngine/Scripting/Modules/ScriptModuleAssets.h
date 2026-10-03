#pragma once

#include "quickjs.h"

#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace WallpaperEngine::Scripting::Modules {
/**
 * Native resolves an import it does not otherwise know as the
 * asset "scripts/jsmodules/" + lowercase (name), appending ".js" unless the
 * name already contains it (2.7.3 native; the 2.8.42 DLL
 * carries the same "scripts/jsmodules/" and ".js" literals).
 */
std::string scriptModuleAssetPath (std::string_view name);

/**
 * QuickJS module-name normalizer. Native lowercases every import name before
 * its module cache lookup, so 'WEMath' and 'wemath' are one module.
 */
char* normalizeScriptModuleName (JSContext* context, const char* baseName, const char* name, void* opaque);

/**
 * Loads shipped or project JavaScript modules through the asset filesystem.
 * One compiled module is retained per resolved path, as native caches by the
 * lowercased name, so differently cased imports share one instance.
 */
class ScriptModuleAssets {
public:
    /** Returns the module source for an asset path, or throws if it does not exist. */
    using Reader = std::function<std::string (const std::string& path)>;

    explicit ScriptModuleAssets (Reader reader);

    /** Returns nullptr with a pending JS exception when the module cannot be loaded. */
    JSModuleDef* load (JSContext* context, const char* name);

private:
    Reader m_reader;
    std::map<std::string, JSModuleDef*> m_modules;
};
}
