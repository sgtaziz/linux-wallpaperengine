#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Scripting/Adapters/VectorMath.h"
#include "WallpaperEngine/Scripting/EngineObject.h"
#include "WallpaperEngine/Scripting/MediaEventPayloads.h"
#include "WallpaperEngine/Scripting/Modules/ScriptModuleAssets.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

JSValue engine_open_user_shortcut (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue engine_set_value (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
JSValue engine_is_running_in_editor (JSContext*, JSValueConst, int, JSValueConst*);
JSValue engine_is_mobile_device (JSContext*, JSValueConst, int, JSValueConst*);
JSValue engine_is_desktop_device (JSContext*, JSValueConst, int, JSValueConst*);
JSValue engine_is_wallpaper (JSContext*, JSValueConst, int, JSValueConst*);
JSValue engine_is_screensaver (JSContext*, JSValueConst, int, JSValueConst*);
JSValue engine_is_object_valid (JSContext*, JSValueConst, int, JSValueConst*);

using WallpaperEngine::Scripting::MediaEventPayloads;
using WallpaperEngine::Scripting::UserCommandScope;
using WallpaperEngine::Scripting::Modules::normalizeScriptModuleName;
using WallpaperEngine::Scripting::MediaPalette;
using WallpaperEngine::Scripting::Modules::ScriptModuleAssets;
using WallpaperEngine::Scripting::Modules::scriptModuleAssetPath;
namespace Adapters = WallpaperEngine::Scripting::Adapters;

namespace {
struct Context {
    JSRuntime* runtime = JS_NewRuntime ();
    JSContext* context = JS_NewContext (runtime);
    ~Context () {
        JS_FreeContext (context);
        JS_FreeRuntime (runtime);
    }

    std::string string (JSValueConst object, const char* key) const {
        JSValue value = JS_GetPropertyStr (context, object, key);
        REQUIRE (JS_IsString (value));
        const char* chars = JS_ToCString (context, value);
        std::string result = chars ? chars : "";
        JS_FreeCString (context, chars);
        JS_FreeValue (context, value);
        return result;
    }

    std::vector<std::string> keys (JSValueConst object) const {
        JSPropertyEnum* properties = nullptr;
        uint32_t count = 0;
        REQUIRE (JS_GetOwnPropertyNames (context, &properties, &count, object, JS_GPN_STRING_MASK) == 0);
        std::vector<std::string> result;
        for (uint32_t i = 0; i < count; ++i) {
            const char* name = JS_AtomToCString (context, properties[i].atom);
            result.emplace_back (name);
            JS_FreeCString (context, name);
        }
        JS_FreePropertyEnum (context, properties, count);
        return result;
    }
};

JSModuleDef* loadFromAssets (JSContext* context, const char* name, void* opaque) {
    return static_cast<ScriptModuleAssets*> (opaque)->load (context, name);
}

void evaluateModule (JSContext* context, const std::string& source) {
    JSValue result = JS_Eval (context, source.c_str (), source.size (), "<authored>", JS_EVAL_TYPE_MODULE);
    REQUIRE_FALSE (JS_IsException (result));
    REQUIRE (JS_PromiseState (context, result) == JS_PROMISE_FULFILLED);
    JS_FreeValue (context, result);
}

double globalNumber (JSContext* context, const char* name) {
    JSValue global = JS_GetGlobalObject (context);
    JSValue value = JS_GetPropertyStr (context, global, name);
    double number = 0.0;
    REQUIRE (JS_ToFloat64 (context, &number, value) == 0);
    JS_FreeValue (context, value);
    JS_FreeValue (context, global);
    return number;
}
}

TEST_CASE ("Media properties events carry the seven native string fields with empty defaults",
           "[script][media]") {
    Context js;
    WallpaperEngine::Media::MediaSource::MediaInfo media {};
    media.title = "Track";
    media.artist = "Performer";
    media.album = "Record";
    media.albumArtist = "Various Artists";
    const MediaPalette palette {};
    MediaEventPayloads events (js.context, media, palette,
                               [&js] (const glm::vec3&) { return JS_NewObject (js.context); });

    // native always sends this set; native builds
    // the event object in this key order.
    const std::vector<std::string> expected {"title", "artist", "albumArtist", "albumTitle",
                                             "subTitle", "genres", "contentType"};
    REQUIRE (js.keys (events.properties ()) == expected);
    REQUIRE (js.string (events.properties (), "title") == "Track");
    REQUIRE (js.string (events.properties (), "albumArtist") == "Various Artists");
    REQUIRE (js.string (events.properties (), "albumTitle") == "Record");
    REQUIRE (js.string (events.properties (), "artist") == "Performer");
    // No Linux source field: the native default is an empty string, not undefined.
    REQUIRE (js.string (events.properties (), "subTitle").empty ());
    REQUIRE (js.string (events.properties (), "genres").empty ());
    REQUIRE (js.string (events.properties (), "contentType").empty ());

    WallpaperEngine::Media::MediaSource::MediaInfo empty {};
    MediaEventPayloads defaults (js.context, empty, palette,
                                 [&js] (const glm::vec3&) { return JS_NewObject (js.context); });
    for (const auto& key : expected) REQUIRE (js.string (defaults.properties (), key.c_str ()).empty ());
}

TEST_CASE ("SceneScript module imports resolve to lowercased shipped jsmodules assets", "[script][module]") {
    REQUIRE (scriptModuleAssetPath ("WEVector") == "scripts/jsmodules/wevector.js");
    REQUIRE (scriptModuleAssetPath ("WEMath") == "scripts/jsmodules/wemath.js");
    REQUIRE (scriptModuleAssetPath ("Helpers.js") == "scripts/jsmodules/helpers.js");
}

TEST_CASE ("SceneScript asset modules load their own imports and share one instance per path",
           "[script][module]") {
    Context js;
    std::vector<std::string> requested;
    // Same import graph as the shipped wevector.js, which imports WEMath.
    const std::map<std::string, std::string> assets {
        {"scripts/jsmodules/wevector.js",
         "import * as WEMath from 'WEMath';\n"
         "export let loads = 0; loads++;\n"
         "export function vectorAngle2(d) { return Math.atan2(d.y, d.x) * WEMath.rad2deg; }\n"},
        {"scripts/jsmodules/wemath.js", "export let rad2deg = 180 / Math.PI;\n"},
    };
    ScriptModuleAssets modules ([&] (const std::string& path) {
        requested.push_back (path);
        const auto found = assets.find (path);
        if (found == assets.end ())
            throw std::filesystem::filesystem_error ("missing", path, std::make_error_code (std::errc::no_such_file_or_directory));
        return found->second;
    });
    JS_SetModuleLoaderFunc (js.runtime, normalizeScriptModuleName, loadFromAssets, &modules);

    evaluateModule (js.context, "import * as WEVector from 'WEVector';\n"
                                "import * as same from 'wevector';\n"
                                "import * as upper from 'WEVECTOR';\n"
                                "if (upper !== same || WEVector !== same) throw new Error('two instances');\n"
                                "globalThis.angle = WEVector.vectorAngle2({x: 0, y: 2});\n"
                                "globalThis.loads = same.loads;\n");
    REQUIRE (globalNumber (js.context, "angle") == 90.0);
    REQUIRE (globalNumber (js.context, "loads") == 1.0);
    REQUIRE (requested == std::vector<std::string> {"scripts/jsmodules/wevector.js", "scripts/jsmodules/wemath.js"});

    // A module without an asset raises a script exception instead of aborting.
    const std::string missing = "import * as Missing from 'WEMissing';\n";
    JSValue result = JS_Eval (js.context, missing.c_str (), missing.size (), "<missing>", JS_EVAL_TYPE_MODULE);
    const bool failed = JS_IsException (result) || JS_PromiseState (js.context, result) == JS_PROMISE_REJECTED;
    REQUIRE (failed);
    JS_FreeValue (js.context, result);
    JS_FreeValue (js.context, JS_GetException (js.context));
    REQUIRE (requested.back () == "scripts/jsmodules/wemissing.js");
}

TEST_CASE ("SceneScript vector helpers follow shipped baseclasses receiver contracts", "[script][vector]") {
    REQUIRE (Adapters::lengthSqrVectorValue (glm::vec2 (3, 4)) == 25.0f);
    REQUIRE (Adapters::lengthSqrVectorValue (glm::vec3 (1, 2, 2)) == 9.0f);
    REQUIRE (Adapters::lengthSqrVectorValue (glm::vec4 (1, 1, 1, 1)) == 4.0f);
    REQUIRE (Adapters::dotVectorValue (glm::vec3 (1, 2, 3), glm::vec3 (4, 5, 6)) == 32.0f);
    // this x f, not f x this.
    REQUIRE (Adapters::crossVectorValue (glm::vec3 (1, 0, 0), glm::vec3 (0, 1, 0)) == glm::vec3 (0, 0, 1));
    // The cursor bounce in 2955378002 reflects its direction on new Vec3(1, 0).
    REQUIRE (Adapters::reflectVectorValue (glm::vec3 (3, -2, 0), glm::vec3 (1, 0, 0)) == glm::vec3 (-3, -2, 0));
    REQUIRE (Adapters::reflectVectorValue (glm::vec2 (1, 1), glm::vec2 (0, 1)) == glm::vec2 (1, -1));
    // reflect does not normalize its argument: this - f * 2 * dot (this, f).
    REQUIRE (Adapters::reflectVectorValue (glm::vec2 (1, 0), glm::vec2 (2, 0)) == glm::vec2 (-7, 0));
}

namespace {
bool scriptTrue (Context& js, const char* source) {
    JSValue result = JS_Eval (js.context, source, std::char_traits<char>::length (source), "<engine>",
                              JS_EVAL_TYPE_GLOBAL);
    REQUIRE_FALSE (JS_IsException (result));
    const bool value = JS_IsBool (result) && JS_ToBool (js.context, result);
    JS_FreeValue (js.context, result);
    return value;
}

void defineFunction (Context& js, JSValueConst target, const char* name, JSCFunction* function, int length) {
    JS_SetPropertyStr (js.context, target, name, JS_NewCFunction (js.context, function, name, length));
}
}

TEST_CASE ("Engine capability bindings report a Linux desktop wallpaper", "[script][engine]") {
    Context js;
    JSValue global = JS_GetGlobalObject (js.context);
    JSValue engine = JS_NewObject (js.context);
    defineFunction (js, engine, "isRunningInEditor", engine_is_running_in_editor, 0);
    defineFunction (js, engine, "isMobileDevice", engine_is_mobile_device, 0);
    defineFunction (js, engine, "isDesktopDevice", engine_is_desktop_device, 0);
    defineFunction (js, engine, "isWallpaper", engine_is_wallpaper, 0);
    defineFunction (js, engine, "isScreensaver", engine_is_screensaver, 0);
    defineFunction (js, engine, "isObjectValid", engine_is_object_valid, 1);
    JSAtom atom = JS_NewAtom (js.context, "runtime");
    JS_DefinePropertyGetSet (js.context, engine, atom, JS_NewCFunction (js.context, engine_is_wallpaper, "get", 0),
                             JS_NewCFunction (js.context, engine_set_value, "set", 1), JS_PROP_ENUMERABLE);
    JS_FreeAtom (js.context, atom);
    JS_SetPropertyStr (js.context, global, "engine", engine);
    JS_FreeValue (js.context, global);

    // 2.7.3 native registrations; true is root 0x2d8, false 0x2e0.
    REQUIRE (scriptTrue (js, "engine.isRunningInEditor() === false"));
    REQUIRE (scriptTrue (js, "engine.isMobileDevice() === false"));
    REQUIRE (scriptTrue (js, "engine.isDesktopDevice() === true"));
    REQUIRE (scriptTrue (js, "engine.isWallpaper() === true"));
    REQUIRE (scriptTrue (js, "engine.isScreensaver() === false"));
    // Only a live layer wrapper is a valid object.
    REQUIRE (scriptTrue (js, "engine.isObjectValid() === false && engine.isObjectValid({}) === false"));
    // A read-only engine accessor raises a catchable TypeError, not an empty exception.
    REQUIRE (scriptTrue (js, "(() => { try { engine.runtime = 1; } catch (e) { return e instanceof TypeError; }"
                             " return false; })()"));
}

TEST_CASE ("openUserShortcut is limited to cursor button callbacks and runs nothing on Linux",
           "[script][engine]") {
    REQUIRE (UserCommandScope::allows ("cursorClick"));
    REQUIRE (UserCommandScope::allows ("cursorDown"));
    REQUIRE (UserCommandScope::allows ("cursorUp"));
    REQUIRE_FALSE (UserCommandScope::allows ("cursorMove"));
    REQUIRE_FALSE (UserCommandScope::allows ("cursorEnter"));
    REQUIRE_FALSE (UserCommandScope::allows ("update"));

    Context js;
    JSValue global = JS_GetGlobalObject (js.context);
    JSValue engine = JS_NewObject (js.context);
    defineFunction (js, engine, "openUserShortcut", engine_open_user_shortcut, 0);
    JS_SetPropertyStr (js.context, global, "engine", engine);
    JS_FreeValue (js.context, global);

    REQUIRE (scriptTrue (js, "(() => { try { engine.openUserShortcut('shortcut'); } catch (e) {"
                             " return e.message === 'Cannot execute user command outside of cursor callbacks.'; }"
                             " return false; })()"));
    {
        UserCommandScope scope (js.context);
        REQUIRE (scriptTrue (js, "engine.openUserShortcut() === false"));
        REQUIRE (scriptTrue (js, "engine.openUserShortcut(3) === false"));
        REQUIRE (scriptTrue (js, "engine.openUserShortcut('shortcut') === false"));
    }
    // The scope belongs to one context and ends with its callback.
    REQUIRE (scriptTrue (js, "(() => { try { engine.openUserShortcut('shortcut'); } catch (e) { return true; }"
                             " return false; })()"));
}
