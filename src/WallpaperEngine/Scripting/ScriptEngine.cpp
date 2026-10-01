#include "ScriptEngine.h"

#include "Adapters/ScriptableObjectAdapter.h"
#include "EffectThisObject.h"
#include "MaterialThisObject.h"
#include "PropertyAnimationThisObject.h"
#include "Modules/ColorModule.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "Modules/MathModule.h"
#include "Modules/ScriptModule.h"
#include "MediaEventPayloads.h"
#include "ScriptPropertiesObject.h"
#include "ScriptableObject.h"
#include "TextureAnimationObject.h"
#include "WallpaperEngine/Audio/AudioContext.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/PlaybackRecorder.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Render/Objects/CSound.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Scripting/Builtins.generated.h"
#include "quickjs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <future>
#include <optional>
#include <poll.h>
#include <ranges>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace WallpaperEngine::Render::Objects {
class CSound;
}
using namespace WallpaperEngine::Scripting;
using namespace WallpaperEngine::Data::Model;

extern char** environ;
extern float g_Time;
extern float g_TimeLast;

static void logJSException (JSContext* ctx, const char* context);

void scriptengine_dump (JSContext* ctx, JSValueConst obj) {
    JSPropertyEnum* props;
    uint32_t len;

    if (JS_GetOwnPropertyNames (ctx, &props, &len, obj, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0) {
	return;
    }

    for (uint32_t i = 0; i < len; ++i) {
	const char* name = JS_AtomToCString (ctx, props[i].atom);

	JSValue val = JS_GetProperty (ctx, obj, props[i].atom);

	const char* value_str = JS_ToCString (ctx, val);

	printf ("%s = %s\n", name, value_str ? value_str : "<non-string>");

	JS_FreeCString (ctx, value_str);
	JS_FreeValue (ctx, val);
	JS_FreeCString (ctx, name);
    }

    js_free (ctx, props);
}

JSModuleDef* scriptengine_module_loader (JSContext* ctx, const char* module, void* opaque) {
    const auto* scriptEngine = static_cast<ScriptEngine*> (opaque);

    const auto& modules = scriptEngine->getModules ();
    const auto it = modules.find (module);

    if (it == modules.end ()) {
	return nullptr;
    }

    return it->second->getDefinition ();
}

JSValue ScriptEngine::dynamicToJs (DynamicValue& value, bool detached, bool angleProperty, bool rgbColorProperty) const {
    // Native angles metadata flag 4 exposes degrees to SceneScript while
    // JSON and renderer matrices retain radians (1401e0530/1401dd630).
    if (angleProperty && value.getType () == DynamicValue::Vec3) {
        DynamicValue degrees (glm::degrees (value.getVec3 ()));
        return this->m_adapters.vec3->instantiate (degrees, true);
    }
    if (rgbColorProperty && value.getType () == DynamicValue::Vec4) {
        DynamicValue rgb (glm::vec3 (value.getVec4 ()));
        return this->m_adapters.vec3->instantiate (rgb, true);
    }
    switch (value.getType ()) {
	case DynamicValue::Null:
	    return JS_NULL;
	case DynamicValue::String:
	    return JS_NewStringLen (this->m_context, value.getString ().data (), value.getString ().size ());
	case DynamicValue::Float:
	    return JS_NewFloat64 (this->m_context, value.getFloat ());
	case DynamicValue::Int:
	    return JS_NewInt32 (this->m_context, value.getInt ());
	case DynamicValue::Boolean:
	    return JS_NewBool (this->m_context, value.getBool ());
	case DynamicValue::Vec2:
	    return detached ? this->m_adapters.vec2->instantiate (value, true)
	                    : this->m_adapters.vec2->instantiate (value);
	case DynamicValue::Vec3:
	    return detached ? this->m_adapters.vec3->instantiate (value, true)
	                    : this->m_adapters.vec3->instantiate (value);
	case DynamicValue::Vec4:
	    return detached ? this->m_adapters.vec4->instantiate (value, true)
	                    : this->m_adapters.vec4->instantiate (value);
	default:
	    return JS_UNDEFINED;
    }
}

static void jsToDynamicValue (JSContext* ctx, JSValue val, DynamicValue& source,
                              const std::string& key, bool rgbColorResult = false, bool angleProperty = false) {
    if (JS_IsException (val)) {
	return;
    }

    // scalar types returned directly
    int tag = JS_VALUE_GET_TAG (val);

    // A script may mutate thisLayer directly and return nothing. In that
    // case the registered value already holds the change.
    if (tag == JS_TAG_UNDEFINED || tag == JS_TAG_UNINITIALIZED) return;

    if (tag == JS_TAG_NULL) {
	source.update (DynamicValue::UpdateSource::Script);
	return;
    }

    if (tag == JS_TAG_INT) {
	source.update (JS_VALUE_GET_INT (val), DynamicValue::UpdateSource::Script);
	return;
    }

    if (tag == JS_TAG_BOOL) {
	source.update (static_cast<bool> (JS_VALUE_GET_BOOL (val)), DynamicValue::UpdateSource::Script);
	return;
    }

    if (JS_TAG_IS_FLOAT64 (tag)) {
	source.update (static_cast<float> (JS_VALUE_GET_FLOAT64 (val)), DynamicValue::UpdateSource::Script);
	return;
    }

    if (tag == JS_TAG_STRING) {
	size_t length = 0;
	const char* str = JS_ToCStringLen (ctx, &length, val);
	if (!str) {
	    logJSException (ctx, "script string result");
	    return;
	}
	source.update (std::string (str, length), DynamicValue::UpdateSource::Script);
	JS_FreeCString (ctx, str);
	return;
    }

    // A returned vector must update the same typed DynamicValue that the
    // renderer reads. Only inspect components required by the source type;
    // vector adapters report unsupported component names as exceptions.
    if (tag == JS_TAG_OBJECT) {
	const auto type = source.getType ();
	if (type != DynamicValue::Vec2 && type != DynamicValue::Vec3 && type != DynamicValue::Vec4) {
	    sLog.error ("Script returned a vector for a non-vector value");
	    return;
	}
	double components[4] {};
	const int count = type == DynamicValue::Vec2 ? 2 : type == DynamicValue::Vec3 ? 3 : 4;
	for (int i = 0; i < count; ++i) {
	    const char name[] = {"xyzw"[i], '\0'};
	    JSValue component = JS_GetPropertyStr (ctx, val, name);
	    if (JS_IsException (component)) {
		JSValue exception = JS_GetException (ctx);
		JS_FreeValue (ctx, exception);
		JS_FreeValue (ctx, component);
		sLog.error ("Script vector result threw while reading component ", "xyzw"[i], " in ", key);
		return;
	    }
	    // Authored color callbacks commonly return Vec3 RGB even though the
	    // parsed color setting retains an alpha channel. Keep that channel;
	    // a Vec4 return still replaces all four components. Other Vec4
	    // properties continue to require an explicit w component.
	    if (i == 3 && rgbColorResult && JS_IsUndefined (component)) {
		components[i] = source.getVec4 ().a;
		JS_FreeValue (ctx, component);
		continue;
	    }
	    const bool valid = JS_IsNumber (component) && JS_ToFloat64 (ctx, &components[i], component) == 0
			       && std::isfinite (components[i]);
	    JS_FreeValue (ctx, component);
	    if (!valid) {
		sLog.error ("Script vector result has an invalid component ", "xyzw"[i], " in ", key);
		return;
	    }
	}
	if (count == 2) source.update (glm::vec2 (components[0], components[1]), DynamicValue::Script);
	else if (count == 3)
	    source.update (angleProperty
                ? glm::radians (glm::vec3 (components[0], components[1], components[2]))
                : glm::vec3 (components[0], components[1], components[2]), DynamicValue::Script);
	else source.update (glm::vec4 (components[0], components[1], components[2], components[3]), DynamicValue::Script);
    }
}

static bool acceptsRgbColorResult (const std::string& key) {
    // A particle's instance color may be connected to a project color (Vec4),
    // while its script and renderer both use RGB Vec3. Keep the stored alpha.
    return key.starts_with ("color_") || key.ends_with ("_constant_color")
        || (key.starts_with ("particle") && key.ends_with ("_instance_colorn"));
}

ScriptEngine::ScriptEngine (Wallpapers::CScene& scene, Media::MediaSource& mediaSource) :
    m_scene (scene), m_mediaSource (mediaSource) {
    this->m_unregisterMediaUpdateCallback
	= mediaSource.addMetadataListener ([this] (const Media::MediaSource::MediaInfo& info) {
	      this->notifyMediaUpdate (info);
	  });

    this->m_unregisterAlbumArtUpdateCallback
	= mediaSource.addAlbumArtListener ([this] (const Media::MediaSource::MediaInfo& info) {
	      // TODO: SEPARATE THESE INTO THEIR OWN UPDATES SO JS ONLY RECEIVES THE MEANINGFUL UPDATES
	      this->notifyMediaUpdate (info);
	  });

    this->m_runtime = JS_NewRuntime ();

    if (!this->m_runtime) {
	sLog.exception ("ScriptEngine: Failed to create JS runtime");
    }

    // debug leaks on termination
    JS_SetDumpFlags (this->m_runtime, JS_DUMP_LEAKS);

    this->m_context = JS_NewContext (this->m_runtime);

    if (!this->m_context) {
	JS_FreeRuntime (this->m_runtime);
	sLog.exception ("ScriptEngine: Failed to create JS context");
    }

    JS_SetContextOpaque (this->m_context, this);

    this->m_globalThis = JS_GetGlobalObject (this->m_context);

    this->m_adapters = {
	.vec4 = std::unique_ptr<Adapters::VectorAdapter<4>> (new Adapters::VectorAdapter<4> (*this)),
	.vec3 = std::unique_ptr<Adapters::VectorAdapter<3>> (new Adapters::VectorAdapter<3> (*this)),
	.vec2 = std::unique_ptr<Adapters::VectorAdapter<2>> (new Adapters::VectorAdapter<2> (*this)),
	.object
	= std::unique_ptr<Adapters::ScriptableObjectAdapter> (new Adapters::ScriptableObjectAdapter (*this, "ILayer")),
        .textureAnimation = std::make_unique<TextureAnimationObject> (this->m_context),
    };

    this->m_engineObject = std::make_unique<EngineObject> (*this, scene);
    this->m_inputObject = std::make_unique<InputObject> (*this, scene);
    this->m_localStorageObject = std::make_unique<LocalStorageObject> (this->m_context, scene.getScene ().project);
    this->m_sceneObject = std::make_unique<SceneObject> (*this, scene);
    this->m_consoleObject = std::make_unique<ConsoleObject> (*this, scene);
    this->m_scriptPropertiesObject = std::make_unique<ScriptPropertiesObject> (*this, scene);

    auto wemath = std::make_unique<Modules::MathModule> (*this);
    auto wecolor = std::make_unique<Modules::ColorModule> (*this);

    this->m_modules.emplace (wemath->getName (), std::move (wemath));
    this->m_modules.emplace (wecolor->getName (), std::move (wecolor));

    JS_SetModuleLoaderFunc (this->m_runtime, nullptr, scriptengine_module_loader, this);
    // setup scene objects and other things
    this->installBuiltins ();
    // add engine to the global
    JS_DefinePropertyValueStr (
	this->m_context, this->m_globalThis, "engine", this->m_engineObject->getInstance (), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_context, this->m_globalThis, "input", this->m_inputObject->getInstance (), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_context, this->m_globalThis, "localStorage", this->m_localStorageObject->instance (), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_context, this->m_globalThis, "thisScene", this->m_sceneObject->getInstance (), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_context, this->m_globalThis, "console", this->m_consoleObject->getInstance (), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_context, this->m_globalThis, "shared", JS_NewObject (this->m_context), JS_PROP_ENUMERABLE
    );
}

ScriptEngine::~ScriptEngine () {
    this->m_unregisterMediaUpdateCallback ();
    this->m_unregisterAlbumArtUpdateCallback ();
    this->shutdown ();

    JS_FreeValue (this->m_context, this->m_globalThis);

    this->m_consoleObject.reset ();
    this->m_engineObject.reset ();
    this->m_inputObject.reset ();
    this->m_localStorageObject.reset ();
    this->m_sceneObject.reset ();
    this->m_scriptPropertiesObject.reset ();
    this->m_modules.clear ();
    this->m_scriptModules.clear ();

    // QuickJS may keep layer/vector objects in module/global cycles until
    // context or runtime destruction. Release retained JS handles now, but
    // keep their C++ adapters alive through all QuickJS finalization.
    this->m_adapters.object->releaseInstances ();
    this->m_adapters.textureAnimation->releaseInstances ();
    this->m_adapters.vec4->releasePrototype ();
    this->m_adapters.vec3->releasePrototype ();
    this->m_adapters.vec2->releasePrototype ();

    if (this->m_context) {
	JS_FreeContext (this->m_context);
    }
    if (this->m_runtime) {
	JS_RunGC (this->m_runtime);
	JS_FreeRuntime (this->m_runtime);
    }
    this->m_adapters.vec4.reset ();
    this->m_adapters.vec3.reset ();
    this->m_adapters.vec2.reset ();
    this->m_adapters.object.reset ();
    this->m_adapters.textureAnimation.reset ();
}

void ScriptEngine::shutdown () {
    m_animatedValues.clear ();
    if (!this->m_context || this->m_scriptModules.empty ()) return;
    for (auto& module : this->m_scriptModules | std::views::values) {
	this->m_runningModule = &module;
	this->activateLayer (module);
	JSValue result = this->call (module.module, 0, nullptr, "destroy");
	if (JS_IsException (result)) logJSException (this->m_context, "shutdown.destroy");
	JS_FreeValue (this->m_context, result);
	JS_FreeValue (this->m_context, module.module);
	JS_FreeValue (this->m_context, module.layer);
	JS_FreeValue (this->m_context, module.thisObject);
    }
    this->m_scriptModules.clear ();
    this->m_runningModule = nullptr;
    JS_SetPropertyStr (this->m_context, this->m_globalThis, "thisLayer", JS_UNDEFINED);
    JS_SetPropertyStr (this->m_context, this->m_globalThis, "thisObject", JS_UNDEFINED);
}

void ScriptEngine::unregisterObject (const ScriptableObject& object) {
    if (!this->m_context) return;
    for (auto it = m_animatedValues.begin (); it != m_animatedValues.end ();)
        if (it->second.object == &object) it = m_animatedValues.erase (it);
        else ++it;
    m_engineObject->cancelTimersForObject (object);
    JSValue activeLayer = JS_GetPropertyStr (this->m_context, this->m_globalThis, "thisLayer");
    JSValue activeObject = JS_GetPropertyStr (this->m_context, this->m_globalThis, "thisObject");
    bool removedActiveLayer = false;
    bool removedActiveObject = false;
    for (auto it = this->m_scriptModules.begin (); it != this->m_scriptModules.end ();) {
	if (&it->second.object != &object) {
	    ++it;
	    continue;
	}
	if (this->m_runningModule == &it->second) this->m_runningModule = nullptr;
	removedActiveLayer = removedActiveLayer || (JS_IsObject (activeLayer)
		&& JS_VALUE_GET_PTR (activeLayer) == JS_VALUE_GET_PTR (it->second.layer));
	removedActiveObject = removedActiveObject || (JS_IsObject (activeObject)
		&& JS_VALUE_GET_PTR (activeObject) == JS_VALUE_GET_PTR (it->second.thisObject));
	JS_FreeValue (this->m_context, it->second.module);
	JS_FreeValue (this->m_context, it->second.layer);
	JS_FreeValue (this->m_context, it->second.thisObject);
	it = this->m_scriptModules.erase (it);
    }
    JS_FreeValue (this->m_context, activeLayer);
    JS_FreeValue (this->m_context, activeObject);
    if (removedActiveLayer)
	JS_SetPropertyStr (this->m_context, this->m_globalThis, "thisLayer", JS_UNDEFINED);
    if (removedActiveObject)
	JS_SetPropertyStr (this->m_context, this->m_globalThis, "thisObject", JS_UNDEFINED);
    this->m_adapters.object->forgetInstance (object);
    this->m_adapters.textureAnimation->forgetInstance (&object);
}

void ScriptEngine::destroyObjectModules (const ScriptableObject& object) {
    if (!m_context) return;
    auto* previousModule = m_runningModule;
    const bool previousRemoved = previousModule && &previousModule->object == &object;
    JSValue previousLayer = JS_GetPropertyStr (m_context, m_globalThis, "thisLayer");
    JSValue previousObject = JS_GetPropertyStr (m_context, m_globalThis, "thisObject");
    ScopeGuard restore ([this, previousModule, previousRemoved, previousLayer, previousObject] {
        m_runningModule = previousRemoved ? nullptr : previousModule;
        if (previousRemoved) {
            JS_FreeValue (m_context, previousLayer);
            JS_SetPropertyStr (m_context, m_globalThis, "thisLayer", JS_UNDEFINED);
            JS_FreeValue (m_context, previousObject);
            JS_SetPropertyStr (m_context, m_globalThis, "thisObject", JS_UNDEFINED);
        } else {
            JS_SetPropertyStr (m_context, m_globalThis, "thisLayer", previousLayer);
            JS_SetPropertyStr (m_context, m_globalThis, "thisObject", previousObject);
        }
    });
    std::vector<std::string> keys;
    for (const auto& [key, module] : m_scriptModules)
        if (&module.object == &object) keys.push_back (key);
    for (const auto& key : keys) {
        const auto found = m_scriptModules.find (key);
        if (found == m_scriptModules.end ()) continue;
        auto& module = found->second;
        m_runningModule = &module;
        activateLayer (module);
        JSValue result = call (module.module, 0, nullptr, "destroy");
        if (JS_IsException (result))
            logJSException (m_context, ("destroyLayer.destroy:" + key).c_str ());
        JS_FreeValue (m_context, result);
    }
    unregisterObject (object);
}

void ScriptEngine::dispatchCursorEvent (const ScriptableObject& object, const char* event,
                                        const glm::vec3& world, const glm::vec3& local) {
    if (!m_context) return;
    std::vector<std::string> keys;
    for (const auto& [key, module] : m_scriptModules)
        if (&module.object == &object) keys.push_back (key);
    auto* previousModule = m_runningModule;
    JSValue previousLayer = JS_GetPropertyStr (m_context, m_globalThis, "thisLayer");
    JSValue previousObject = JS_GetPropertyStr (m_context, m_globalThis, "thisObject");
    ScopeGuard restore ([this, previousModule, previousLayer, previousObject] {
        m_runningModule = previousModule;
        JS_SetPropertyStr (m_context, m_globalThis, "thisLayer", previousLayer);
        JS_SetPropertyStr (m_context, m_globalThis, "thisObject", previousObject);
    });
    auto vectorValue = [this] (const glm::vec3& vector) {
        JSValue value = m_adapters.vec3->instantiate ();
        JS_SetPropertyStr (m_context, value, "x", JS_NewFloat64 (m_context, vector.x));
        JS_SetPropertyStr (m_context, value, "y", JS_NewFloat64 (m_context, vector.y));
        JS_SetPropertyStr (m_context, value, "z", JS_NewFloat64 (m_context, vector.z));
        return value;
    };
    for (const auto& key : keys) {
        const auto found = m_scriptModules.find (key);
        if (found == m_scriptModules.end ()) continue;
        auto& module = found->second;
        m_runningModule = &module;
        activateLayer (module);
        JSValue payload = JS_NewObject (m_context);
        JS_SetPropertyStr (m_context, payload, "worldPosition", vectorValue (world));
        JS_SetPropertyStr (m_context, payload, "localPosition", vectorValue (local));
        JS_SetPropertyStr (m_context, payload, "hitBox", JS_NULL);
        JSValue args[] = {payload};
        JSValue result = call (module.module, 1, args, event);
        if (JS_IsException (result))
            logJSException (m_context, (std::string ("cursor.") + event + ":" + key).c_str ());
        JS_FreeValue (m_context, result);
        JS_FreeValue (m_context, payload);
    }
}

/// Helper to check for and log JS exceptions
static void logJSException (JSContext* ctx, const char* context) {
    JSValue exc = JS_GetException (ctx);
    if (!JS_IsNull (exc) && !JS_IsUndefined (exc)) {
	const char* str = JS_ToCString (ctx, exc);
	if (str) {
	    sLog.error ("ScriptEngine [", context, "]: ", str);
	    JS_FreeCString (ctx, str);
	}
    }
    JS_FreeValue (ctx, exc);
}

void ScriptEngine::installBuiltins () {
    if (this->m_builtinsInstalled || !this->m_context) {
	return;
    }

    JSValue result = JS_Eval (
	this->m_context, SCENE_SCRIPT_BUILTINS, strlen (SCENE_SCRIPT_BUILTINS), "<scene-script-builtins>",
	JS_EVAL_TYPE_GLOBAL
    );
    if (JS_IsException (result)) {
	logJSException (this->m_context, "installBuiltins");
    }
    JS_FreeValue (this->m_context, result);
    this->m_builtinsInstalled = true;
}

// ---------------------------------------------------------------------------
// Layer-script API (Phase 2)
// ---------------------------------------------------------------------------

void ScriptEngine::ensureLayerRegistry () {
    if (this->m_layerRegistryReady || !this->m_context) {
	return;
    }
    JSContext* ctx = this->m_context;
    JSValue globalObj = JS_GetGlobalObject (ctx);
    JS_SetPropertyStr (ctx, globalObj, "__textLayers", JS_NewObject (ctx));
    JS_FreeValue (ctx, globalObj);
    this->m_layerRegistryReady = true;
}

ScriptLayerHandle ScriptEngine::createLayerScript (
    const std::string& scriptSource, std::map<std::string, UserSettingUniquePtr>& initialScriptProps,
    const std::string& initialText
) {
    if (!this->m_context) {
	sLog.error ("ScriptEngine: No JS context available");
	return kInvalidLayerHandle;
    }

    this->ensureLayerRegistry ();

    JSContext* ctx = this->m_context;
    JSValue globalObj = JS_GetGlobalObject (ctx);

    // Seed initial scriptProperties and text as temporary globals the IIFE reads.
    JSValue seedProps = JS_NewObject (ctx);

    for (auto& [name, dynVal] : initialScriptProps) {
	JS_SetPropertyStr (ctx, seedProps, name.c_str (), this->dynamicToJs (*dynVal->value));
    }

    JS_SetPropertyStr (ctx, globalObj, "__layerSeedProps", seedProps);
    JS_SetPropertyStr (ctx, globalObj, "__layerSeedText", JS_NewString (ctx, initialText.c_str ()));

    const ScriptLayerHandle id = this->m_nextLayerId++;

    // Same stripping logic as evaluate(): WE scripts come as ES6 modules but
    // QuickJS is easier to drive as plain script evaluation.
    std::string body = scriptSource;
    size_t pos;
    while ((pos = body.find ("'use strict';")) != std::string::npos) {
	body.erase (pos, 13);
    }
    while ((pos = body.find ("\"use strict\";")) != std::string::npos) {
	body.erase (pos, 13);
    }
    while ((pos = body.find ("export ")) != std::string::npos) {
	body.erase (pos, 7);
    }

    // The IIFE gives every layer its own closure for top-level vars and
    // functions, so two layers that both define `function update()` or a
    // top-level `var scriptProperties` don't clobber each other. Lifecycle
    // hooks are captured into globalThis.__textLayers[id] so tick/destroy can
    // reach them later. `typeof init === 'function'` is safe even when
    // `init` was never declared — bare-identifier `typeof` never throws.
    std::ostringstream wrapper;
    wrapper
	<< "(function() {\n"
	<< "  var __id = " << id << ";\n"
	<< "  var __props = Object.assign({}, globalThis.__layerSeedProps || {});\n"
	<< "  var thisLayer = { text: String(globalThis.__layerSeedText || '') };\n"
	<< "  var thisScene = {\n"
	<< "    get time()        { var c = globalThis.__sceneCtx; return c ? c.time : 0; },\n"
	<< "    get currentTime() { var c = globalThis.__sceneCtx; return c ? c.time : 0; },\n"
	<< "    get dt()          { var c = globalThis.__sceneCtx; return c ? c.dt   : 0; },\n"
	<< "    get fps()         { var c = globalThis.__sceneCtx; return c ? c.fps  : 60; },\n"
	<< "  };\n"
	// Minimal WE `engine` shim. Real Wallpaper Engine exposes a broad API
	// (media events, audio buffer, user input); we provide just enough for
	// the common built-in text scripts to run without ReferenceError.
	// `frametime` is the per-frame delta in seconds (what InsertFPS reads).
	<< "  var engine = {\n"
	<< "    get frametime() { var c = globalThis.__sceneCtx; return c ? c.dt : 0; },\n"
	<< "    get time()      { var c = globalThis.__sceneCtx; return c ? c.time : 0; },\n"
	<< "  };\n"
	<< "  function createScriptProperties() {\n"
	<< "    var builder = {\n"
	<< "      addSlider:   function(o){ if (!(o.name in __props)) __props[o.name] = o.value; return builder; },\n"
	<< "      addCheckbox: function(o){ if (!(o.name in __props)) __props[o.name] = o.value; return builder; },\n"
	<< "      addCombo:    function(o){ if (!(o.name in __props)) __props[o.name] = o.value; return builder; },\n"
	<< "      addColor:    function(o){ if (!(o.name in __props)) __props[o.name] = o.value; return builder; },\n"
	<< "      addText:     function(o){ if (!(o.name in __props)) __props[o.name] = o.value; return builder; },\n"
	<< "      finish:      function(){ return __props; }\n"
	<< "    };\n"
	<< "    return builder;\n"
	<< "  }\n"
	<< body
	<< "\n"
	// `_tick` wraps the user's `update()` so both WE text conventions work:
	//   A) `export function update() { thisLayer.text = …; }` (mutates in place)
	//   B) `export function update(value) { …; return value; }` (returns new text)
	// We pass the current text in, and if the return value is a string we
	// adopt it as the new `thisLayer.text`. Non-string / undefined return
	// leaves `thisLayer.text` as whatever the function assigned itself.
	<< "  globalThis.__textLayers[__id] = {\n"
	<< "    thisLayer: thisLayer,\n"
	<< "    thisScene: thisScene,\n"
	<< "    _init:    (typeof init    === 'function') ? init    : null,\n"
	<< "    _destroy: (typeof destroy === 'function') ? destroy : null,\n"
	<< "    _tick:    (typeof update  === 'function')\n"
	<< "              ? function() {\n"
	<< "                  var r = update(thisLayer.text);\n"
	<< "                  if (typeof r === 'string') thisLayer.text = r;\n"
	<< "                }\n"
	<< "              : null,\n"
	<< "    _scriptProperties: (typeof scriptProperties !== 'undefined') ? scriptProperties : __props\n"
	<< "  };\n"
	<< "})();\n";

    const std::string evalScript = wrapper.str ();
    JSValue result = JS_Eval (ctx, evalScript.c_str (), evalScript.size (), "<layer-script>", JS_EVAL_TYPE_GLOBAL);

    // Unset seeds so they don't leak into the next createLayerScript call.
    JS_SetPropertyStr (ctx, globalObj, "__layerSeedProps", JS_UNDEFINED);
    JS_SetPropertyStr (ctx, globalObj, "__layerSeedText", JS_UNDEFINED);
    JS_FreeValue (ctx, globalObj);

    if (JS_IsException (result)) {
	logJSException (ctx, "createLayerScript");
	JS_FreeValue (ctx, result);
	return kInvalidLayerHandle;
    }
    JS_FreeValue (ctx, result);

    this->m_layerInitialized[id] = false;
    return id;
}

void ScriptEngine::tickLayer (ScriptLayerHandle handle, double time, double deltaTime, double fps) {
    if (!this->m_context || handle == kInvalidLayerHandle) {
	return;
    }
    JSContext* ctx = this->m_context;
    JSValue globalObj = JS_GetGlobalObject (ctx);

    JSValue sceneCtx = JS_NewObject (ctx);
    JS_SetPropertyStr (ctx, sceneCtx, "time", JS_NewFloat64 (ctx, time));
    JS_SetPropertyStr (ctx, sceneCtx, "dt", JS_NewFloat64 (ctx, deltaTime));
    JS_SetPropertyStr (ctx, sceneCtx, "fps", JS_NewFloat64 (ctx, fps));
    JS_SetPropertyStr (ctx, globalObj, "__sceneCtx", sceneCtx);

    JSValue layers = JS_GetPropertyStr (ctx, globalObj, "__textLayers");
    JSValue layerObj = JS_GetPropertyUint32 (ctx, layers, static_cast<uint32_t> (handle));
    JS_FreeValue (ctx, layers);

    if (JS_IsUndefined (layerObj) || JS_IsNull (layerObj)) {
	JS_FreeValue (ctx, layerObj);
	JS_FreeValue (ctx, globalObj);
	return;
    }

    auto callHook = [&] (const char* prop, const char* tag) {
	JSValue fn = JS_GetPropertyStr (ctx, layerObj, prop);
	if (JS_IsFunction (ctx, fn)) {
	    JSValue ret = JS_Call (ctx, fn, layerObj, 0, nullptr);
	    if (JS_IsException (ret)) {
		logJSException (ctx, tag);
	    }
	    JS_FreeValue (ctx, ret);
	}
	JS_FreeValue (ctx, fn);
    };

    auto it = this->m_layerInitialized.find (handle);
    if (it != this->m_layerInitialized.end () && !it->second) {
	callHook ("_init", "layer.init");
	it->second = true;
    }
    callHook ("_tick", "layer.update");

    JS_FreeValue (ctx, layerObj);
    JS_FreeValue (ctx, globalObj);
}

std::string ScriptEngine::layerText (ScriptLayerHandle handle) {
    if (!this->m_context || handle == kInvalidLayerHandle) {
	return {};
    }
    JSContext* ctx = this->m_context;
    JSValue globalObj = JS_GetGlobalObject (ctx);
    JSValue layers = JS_GetPropertyStr (ctx, globalObj, "__textLayers");
    JSValue layerObj = JS_GetPropertyUint32 (ctx, layers, static_cast<uint32_t> (handle));

    std::string result;
    if (!JS_IsUndefined (layerObj) && !JS_IsNull (layerObj)) {
	JSValue thisLayer = JS_GetPropertyStr (ctx, layerObj, "thisLayer");
	JSValue textVal = JS_GetPropertyStr (ctx, thisLayer, "text");
	if (!JS_IsUndefined (textVal) && !JS_IsNull (textVal)) {
	    const char* cstr = JS_ToCString (ctx, textVal);
	    if (cstr) {
		result.assign (cstr);
		JS_FreeCString (ctx, cstr);
	    }
	}
	JS_FreeValue (ctx, textVal);
	JS_FreeValue (ctx, thisLayer);
    }

    JS_FreeValue (ctx, layerObj);
    JS_FreeValue (ctx, layers);
    JS_FreeValue (ctx, globalObj);
    return result;
}

void ScriptEngine::destroyLayer (ScriptLayerHandle handle) {
    if (!this->m_context || handle == kInvalidLayerHandle) {
	return;
    }
    JSContext* ctx = this->m_context;
    JSValue globalObj = JS_GetGlobalObject (ctx);
    JSValue layers = JS_GetPropertyStr (ctx, globalObj, "__textLayers");
    JSValue layerObj = JS_GetPropertyUint32 (ctx, layers, static_cast<uint32_t> (handle));

    if (!JS_IsUndefined (layerObj) && !JS_IsNull (layerObj)) {
	JSValue fn = JS_GetPropertyStr (ctx, layerObj, "_destroy");
	if (JS_IsFunction (ctx, fn)) {
	    JSValue ret = JS_Call (ctx, fn, layerObj, 0, nullptr);
	    if (JS_IsException (ret)) {
		logJSException (ctx, "layer.destroy");
	    }
	    JS_FreeValue (ctx, ret);
	}
	JS_FreeValue (ctx, fn);
    }
    JS_FreeValue (ctx, layerObj);
    JS_FreeValue (ctx, layers);
    JS_FreeValue (ctx, globalObj);

    // Remove the entry from globalThis.__textLayers so GC can reclaim its closures.
    const std::string delScript = "delete globalThis.__textLayers[" + std::to_string (handle) + "];";
    JSValue delResult = JS_Eval (ctx, delScript.c_str (), delScript.size (), "<layer-destroy>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException (delResult)) {
	logJSException (ctx, "layer.destroy.delete");
    }
    JS_FreeValue (ctx, delResult);

    this->m_layerInitialized.erase (handle);
}

JSValue ScriptEngine::call (JSValue module, int argc, JSValue argv[], const char* name) {
    // check if there's an update method and run it
    JSValue function = JS_GetPropertyStr (this->m_context, module, name);
    ScopeGuard guard ([&] () { JS_FreeValue (this->m_context, function); });
    if (JS_IsException (function)) return JS_EXCEPTION;

    if (!JS_IsFunction (this->m_context, function)) {
	return JS_UNDEFINED;
    }

    return JS_Call (this->m_context, function, module, argc, argv);
}

void ScriptEngine::activateLayer (const LoadedModule& module) {
    JS_SetPropertyStr (this->m_context, this->m_globalThis, "thisLayer", JS_DupValue (this->m_context, module.layer));
    JS_SetPropertyStr (this->m_context, this->m_globalThis, "thisObject",
                       JS_DupValue (this->m_context, module.thisObject));
}

void ScriptEngine::queueScript (const std::string& key, DynamicValue& currentValue,
                                ScriptableObject& object, std::optional<size_t> effectOwner,
                                const ShaderConstantMap* materialOwner) {
    if (currentValue.getAnimation ())
        m_animatedValues.try_emplace (key, AnimatedValue {&currentValue, &object});
    const auto source = currentValue.getScriptSource ();

    if (!source.has_value ()) {
	return;
    }

    auto it = this->m_scriptModules.find (key);

    if (it != this->m_scriptModules.end ()) {
	return;
    }

    // The layer must exist while module top-level code runs. JS_Eval of a
    // module returns its evaluation result, not its exports; retain the
    // namespace explicitly so update() can be called on later frames.
    JSValue layer = this->m_adapters.object->instantiate (object);
    JSValue thisObject = materialOwner
        ? createMaterialThisObject (this->m_context, layer, *materialOwner)
        : effectOwner.has_value ()
            ? createEffectThisObject (this->m_context, layer, *effectOwner)
            : JS_DupValue (this->m_context, layer);
    if (JS_IsException (thisObject)) {
        logJSException (this->m_context, "queueScript.thisObject");
        JS_FreeValue (this->m_context, layer);
        return;
    }
    ScopeGuard ownerGuard ([this, thisObject] { JS_FreeValue (this->m_context, thisObject); });
    attachPropertyAnimation (this->m_context, thisObject, layer, currentValue);
    JSValue previousLayer = JS_GetPropertyStr (this->m_context, this->m_globalThis, "thisLayer");
    JSValue previousObject = JS_GetPropertyStr (this->m_context, this->m_globalThis, "thisObject");
    ScopeGuard layerGuard ([this, previousLayer, previousObject] {
        JS_SetPropertyStr (this->m_context, this->m_globalThis, "thisLayer", previousLayer);
        JS_SetPropertyStr (this->m_context, this->m_globalThis, "thisObject", previousObject);
    });
    JS_SetPropertyStr (this->m_context, this->m_globalThis, "thisLayer", JS_DupValue (this->m_context, layer));
    JS_SetPropertyStr (this->m_context, this->m_globalThis, "thisObject",
                       JS_DupValue (this->m_context, thisObject));
    LoadedModule candidate {.value = currentValue, .object = object, .module = JS_UNDEFINED,
                            .layer = layer, .thisObject = thisObject,
                            .angleProperty = object.isAngleProperty (currentValue),
                            .rgbColorProperty = object.isRgbColorProperty (currentValue)};
    auto* previousModule = this->m_runningModule;
    this->m_runningModule = &candidate;
    ScopeGuard runningGuard ([this, previousModule] { this->m_runningModule = previousModule; });
    JSValue compiled = JS_Eval (
	this->m_context, source->c_str (), source->size (), key.c_str (), JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY
    );
    if (JS_IsException (compiled)) {
	logJSException (this->m_context, "queueScript.compile");
	JS_FreeValue (this->m_context, layer);
	return;
    }
    if (!JS_IsModule (compiled)) {
	JS_FreeValue (this->m_context, compiled);
	JS_FreeValue (this->m_context, layer);
	sLog.error ("ScriptEngine: compiled script is not a module: ", key);
	return;
    }
    auto* definition = static_cast<JSModuleDef*> (JS_VALUE_GET_PTR (compiled));
    const bool previousTopLevel = m_evaluatingModuleTopLevel;
    m_evaluatingModuleTopLevel = true;
    JSValue evaluation = JS_EvalFunction (this->m_context, compiled); // consumes compiled
    m_evaluatingModuleTopLevel = previousTopLevel;
    if (JS_IsException (evaluation)) {
	logJSException (this->m_context, ("queueScript.evaluate:" + key).c_str ());
	JS_FreeValue (this->m_context, evaluation);
	JS_FreeValue (this->m_context, layer);
	return;
    }
    if (JS_PromiseState (this->m_context, evaluation) == JS_PROMISE_REJECTED) {
	JSValue reason = JS_PromiseResult (this->m_context, evaluation);
	const char* message = JS_ToCString (this->m_context, reason);
	sLog.error ("ScriptEngine [queueScript.evaluate:", key, "]: ",
	            message ? message : "rejected module evaluation");
	if (message) JS_FreeCString (this->m_context, message);
	JS_FreeValue (this->m_context, reason);
	JS_FreeValue (this->m_context, evaluation);
	JS_FreeValue (this->m_context, layer);
	return;
    }
    JS_FreeValue (this->m_context, evaluation);
    JSValue module = JS_GetModuleNamespace (this->m_context, definition);
    if (JS_IsException (module)) {
	logJSException (this->m_context, "queueScript.namespace");
	JS_FreeValue (this->m_context, module);
	JS_FreeValue (this->m_context, layer);
	return;
    }

    auto inserted = this->m_scriptModules.emplace (
	key,
	LoadedModule {
	    .value = currentValue,
	    .object = object,
	    .module = module,
	    .layer = layer,
	    .thisObject = thisObject,
	    .queueOrder = m_nextScriptQueueOrder,
            .angleProperty = object.isAngleProperty (currentValue),
            .rgbColorProperty = object.isRgbColorProperty (currentValue),
	}
    );

    if (!inserted.second) {
	JS_FreeValue (this->m_context, module);
	JS_FreeValue (this->m_context, layer);
	return;
    }
    ownerGuard.cancel ();
    ++m_nextScriptQueueOrder;

    // init/update are deferred until the first scene tick, after all layers
    // have been constructed and can be found through thisScene.getLayer().
}

void ScriptEngine::tick () {
    // Native property timelines advance before their authored script callbacks
    // and before the rendered material reads the resulting DynamicValue.
    const float delta = m_scene.getDeltaTime ();
    for (auto& [key, binding] : m_animatedValues) {
        auto& value = *binding.value;
        auto* animation = value.getAnimation ();
        if (!animation) continue;
        animation->advance (delta);
        const auto type = value.getType ();
        if (type == DynamicValue::Float || type == DynamicValue::Int) {
            if (!animation->channels[0].empty ())
                value.update (animation->sample (0), DynamicValue::Script);
        } else if (type == DynamicValue::Vec2 || type == DynamicValue::Vec3 ||
                   type == DynamicValue::Vec4) {
            glm::vec4 result = value.getVec4 ();
            bool changed = false;
            for (size_t channel = 0; channel < animation->channels.size (); ++channel) {
                if (animation->channels[channel].empty ()) continue;
                result[channel] = animation->sample (channel);
                changed = true;
            }
            if (changed) {
                if (type == DynamicValue::Vec2) value.update (glm::vec2 (result), DynamicValue::Script);
                else if (type == DynamicValue::Vec3) value.update (glm::vec3 (result), DynamicValue::Script);
                else value.update (result, DynamicValue::Script);
            }
        }
    }
    // The preceding frame's module must not own new engine-level timers.
    m_runningModule = nullptr;
    ScopeGuard runningGuard ([this] { m_runningModule = nullptr; });
    // run intervals
    this->m_engineObject->tick ();

    // run any pending notifications

    // A module can create a layer whose setup queues another module. Schedule
    // modules present at the start of the tick, so a child begins on the next
    // frame regardless of its lexical key order in the map.
    std::vector<std::string> scheduled;
    scheduled.reserve (m_scriptModules.size ());
    for (const auto& item : m_scriptModules) scheduled.push_back (item.first);
    // Initialize the frozen set in registration order. An authored child can
    // snapshot a value initialized by its parent even when its property name
    // sorts first lexically (for example color_928 before scale_920).
    std::vector<std::string> pendingInit;
    for (const auto& key : scheduled)
        if (const auto found = m_scriptModules.find (key);
            found != m_scriptModules.end () && !found->second.initialized)
            pendingInit.push_back (key);
    std::sort (pendingInit.begin (), pendingInit.end (), [this] (const auto& left, const auto& right) {
        return m_scriptModules.at (left).queueOrder < m_scriptModules.at (right).queueOrder;
    });
    JSValue initialUserProperties = JS_UNDEFINED;
    ScopeGuard propertiesGuard ([this, &initialUserProperties] {
        JS_FreeValue (m_context, initialUserProperties);
    });
    for (const auto& key : pendingInit) {
	const auto found = m_scriptModules.find (key);
	if (found == m_scriptModules.end ()) continue;
	auto& module = found->second;
	this->m_runningModule = &module;
	this->activateLayer (module);
	if (!module.initialized) {
	    module.initialized = true;
	    JSValue initArgs[] = {this->dynamicToJs (module.value, true, module.angleProperty, module.rgbColorProperty)};
	    JSValue initResult;
            {
                // Native initial authored init precedes the loader's camera
                // pose reset. Later dynamic init and user-properties calls do not.
                const bool previous = m_initialAuthoredInit;
                m_initialAuthoredInit = !m_initialSceneTickCompleted
                    && !m_scene.isScriptCreatedLayer (module.object);
                ScopeGuard initPhase ([this, previous] { m_initialAuthoredInit = previous; });
                initResult = this->call (module.module, 1, initArgs, "init");
            }
	    if (JS_IsException (initResult))
	        logJSException (this->m_context, ("tick.init:" + key).c_str ());
	    else jsToDynamicValue (this->m_context, initResult, module.value, key,
	                           module.rgbColorProperty || acceptsRgbColorResult (key), module.angleProperty);
	    JS_FreeValue (this->m_context, initResult);
	    JS_FreeValue (this->m_context, initArgs[0]);
	    // Native sends all current project settings once when the wallpaper
	    // loads. Several authored scripts initialize their frame state here,
	    // after init(value) has captured the initial layer value.
	    if (JS_IsUndefined (initialUserProperties)) {
	        initialUserProperties = JS_NewObject (m_context);
	        for (const auto& [name, property] : m_scene.getScene ().project.properties) {
	            JSValue field;
	            if (dynamic_cast<Data::Model::PropertyColor*> (property.get ())) {
	                DynamicValue color (property->getVec3 ());
	                field = m_adapters.vec3->instantiate (color, true);
	            } else field = dynamicToJs (*property);
	            if (JS_IsException (field) || JS_SetPropertyStr (
	                    m_context, initialUserProperties, name.c_str (), field) < 0) {
	                logJSException (m_context, "tick.userProperties");
	                break;
	            }
	        }
	    }
	    JSValue userArgs[] = {JS_DupValue (m_context, initialUserProperties)};
	    JSValue userResult = call (module.module, 1, userArgs, "applyUserProperties");
	    if (JS_IsException (userResult))
	        logJSException (m_context, ("tick.applyUserProperties:" + key).c_str ());
	    JS_FreeValue (m_context, userResult);
	    JS_FreeValue (m_context, userArgs[0]);
	}
    }

    m_initialSceneTickCompleted = true;

    // Keep the established update order; only the one-time initialization
    // phase needs authored construction dependencies.
    for (const auto& key : scheduled) {
	const auto found = m_scriptModules.find (key);
	if (found == m_scriptModules.end ()) continue;
	auto& module = found->second;
	this->m_runningModule = &module;
	this->activateLayer (module);

	JSValue args[] = { this->dynamicToJs (module.value, true, module.angleProperty, module.rgbColorProperty) };
	JSValue result = this->call (module.module, 1, args, "update");
	ScopeGuard guard ([result, args, this] () {
	    JS_FreeValue (this->m_context, result);
	    JS_FreeValue (this->m_context, args[0]);
	});

	if (JS_IsException (result)) {
	    logJSException (this->m_context, ("tick.update:" + key).c_str ());
	    continue;
	}

	jsToDynamicValue (this->m_context, result, module.value, key,
	                  module.rgbColorProperty || acceptsRgbColorResult (key), module.angleProperty);
    }
    if (m_mediaSource.getMediaInfo ().available) {
        const bool pending = std::ranges::any_of (m_scriptModules, [] (const auto& item) {
            return item.second.initialized && !item.second.mediaDelivered;
        });
        if (pending) {
            const auto current = m_mediaSource.getMediaInfo ();
            notifyMediaUpdate (current);
        }
    }
}

void ScriptEngine::notifyScreenResize (int width, int height) {
    DynamicValue sizeValue (glm::vec2 (width, height));
    JSValue size = m_adapters.vec2->instantiate (sizeValue, true);
    if (JS_IsException (size)) {
        logJSException (m_context, "resizeScreen.size");
        return;
    }
    ScopeGuard sizeGuard ([this, size] { JS_FreeValue (m_context, size); });
    std::vector<std::string> scheduled;
    scheduled.reserve (m_scriptModules.size ());
    for (const auto& [key, module] : m_scriptModules)
        if (module.initialized) scheduled.push_back (key);
    for (const auto& key : scheduled) {
        const auto found = m_scriptModules.find (key);
        if (found == m_scriptModules.end ()) continue;
        auto& module = found->second;
        m_runningModule = &module;
        activateLayer (module);
        JSValue args[] = {JS_DupValue (m_context, size)};
        JSValue result = call (module.module, 1, args, "resizeScreen");
        m_runningModule = nullptr;
        if (JS_IsException (result))
            logJSException (m_context, ("resizeScreen:" + key).c_str ());
        JS_FreeValue (m_context, result);
        JS_FreeValue (m_context, args[0]);
    }
    m_runningModule = nullptr;
}

void ScriptEngine::notifyMediaUpdate (const Media::MediaSource::MediaInfo& media) {
    JSContext* ctx = this->m_context;
    const auto& previous = m_lastMediaInfo;
    const bool initial = !previous.has_value ();
    const bool propertiesChanged = initial || previous->title != media.title
        || previous->artist != media.artist || previous->album != media.album;
    const bool playbackChanged = initial || previous->playbackState != media.playbackState;
    const bool timelineChanged = initial || previous->position != media.position
        || previous->duration != media.duration;
    const bool thumbnailChanged = initial || previous->url != media.url
        || previous->artwork != media.artwork;
    m_lastMediaInfo = media;
    const bool pending = std::ranges::any_of (m_scriptModules, [] (const auto& item) {
        return item.second.initialized && !item.second.mediaDelivered;
    });
    if (!propertiesChanged && !playbackChanged && !timelineChanged && !thumbnailChanged && !pending) return;
    auto* previousModule = m_runningModule;
    JSValue previousLayer = JS_GetPropertyStr (ctx, m_globalThis, "thisLayer");
    JSValue previousObject = JS_GetPropertyStr (ctx, m_globalThis, "thisObject");
    ScopeGuard restore ([this, previousModule, previousLayer, previousObject] {
        m_runningModule = previousModule;
        JS_SetPropertyStr (m_context, m_globalThis, "thisLayer", previousLayer);
        JS_SetPropertyStr (m_context, m_globalThis, "thisObject", previousObject);
    });

    // The retained owner is the same immutable decoded snapshot used by
    // $mediaThumbnail. Timeline ticks must not rescan the artwork pixels.
    if (m_paletteArtwork != media.artwork) {
        m_paletteArtwork = media.artwork;
        m_cachedPalette = paletteFromArtwork (m_paletteArtwork.get ());
    }
    MediaEventPayloads events (ctx, media, m_cachedPalette, [this] (const glm::vec3& color) {
        DynamicValue value (color);
        return m_adapters.vec3->instantiate (value, true);
    });

    JSValue propertiesArgs[] = { events.properties () };
    JSValue playbackArgs[] = { events.playback () };
    JSValue mediaTimelineArgs[] = { events.timeline () };
    JSValue mediaThumbnailArgs[] = { events.thumbnail () };

	std::vector<std::string> scheduled;
	scheduled.reserve (m_scriptModules.size ());
	for (const auto& item : m_scriptModules) scheduled.push_back (item.first);
	for (const auto& key : scheduled) {
	    const auto found = m_scriptModules.find (key);
	    if (found == m_scriptModules.end ()) continue;
	    auto& module = found->second;
	    if (!module.initialized) continue;
	const bool moduleInitial = !module.mediaDelivered;
	const auto invoke = [this, ctx, &module, &key] (const char* name, JSValue* args) {
	    m_runningModule = &module;
	    activateLayer (module);
	    JSValue result = call (module.module, 1, args, name);
	    if (JS_IsException (result))
	        logJSException (ctx, (std::string ("media.") + name + ":" + key).c_str ());
	    JS_FreeValue (ctx, result);
	};
	if (moduleInitial || propertiesChanged) invoke ("mediaPropertiesChanged", propertiesArgs);
	if (moduleInitial || playbackChanged) invoke ("mediaPlaybackChanged", playbackArgs);
	if (moduleInitial || timelineChanged) invoke ("mediaTimelineChanged", mediaTimelineArgs);
	if (moduleInitial || thumbnailChanged) invoke ("mediaThumbnailChanged", mediaThumbnailArgs);
	module.mediaDelivered = true;
    }

}
