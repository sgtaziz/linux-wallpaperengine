#include "EngineObject.h"
#include "ScriptEngine.h"
#include "SceneScriptClassRegistration.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

#include <filesystem>
#include <array>
#include <cstring>
#include <memory>

using namespace WallpaperEngine::Scripting;

extern float g_Time;
extern float g_TimeLast;
extern float g_Daytime;

namespace {
struct RegisteredAsset {
    const EngineObject* owner;
    std::string path;
    bool precache;
};

void asset_handle_finalizer (JSRuntime*, JSValue value) {
    JSClassID classId = 0;
    delete static_cast<RegisteredAsset*> (JS_GetAnyOpaque (value, &classId));
}

EngineObject* get_engine_object (JSContext* ctx, JSValueConst value) {
    const auto* engine = static_cast<ScriptEngine*> (JS_GetContextOpaque (ctx));
    if (!engine || !engine->getEngineObject ()) return nullptr;
    return static_cast<EngineObject*> (JS_GetOpaque2 (ctx, value, engine->getEngineObject ()->getClassId ()));
}
}

JSValue engine_set_value (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) { return JS_EXCEPTION; }

JSValue engine_open_user_shortcut (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_UNDEFINED;
}

JSValue engine_get_frametime (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_NewFloat64 (ctx, g_Time - g_TimeLast);
}

JSValue engine_get_runtime (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_NewFloat64 (ctx, g_Time);
}

JSValue engine_get_daytime (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_NewFloat64 (ctx, g_Daytime);
}

JSValue engine_get_canvas_size (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* owner = get_engine_object (ctx, this_val);
    if (!owner) return JS_EXCEPTION;
    JSValue size = owner->getEngine ().getAdapters ().vec2->instantiate ();
    if (JS_IsException (size)) return size;
    JS_SetPropertyStr (ctx, size, "x", JS_NewFloat64 (ctx, owner->getScene ().getWidth ()));
    JS_SetPropertyStr (ctx, size, "y", JS_NewFloat64 (ctx, owner->getScene ().getHeight ()));
    return size;
}

JSValue engine_get_screen_resolution (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* owner = get_engine_object (ctx, this_val);
    if (!owner) return JS_EXCEPTION;
    JSValue size = owner->getEngine ().getAdapters ().vec2->instantiate ();
    if (JS_IsException (size)) return size;
    const auto& output = owner->getScene ().getContext ().getOutput ();
    JS_SetPropertyStr (ctx, size, "x", JS_NewFloat64 (ctx, output.getFullWidth ()));
    JS_SetPropertyStr (ctx, size, "y", JS_NewFloat64 (ctx, output.getFullHeight ()));
    return size;
}

JSValue engine_register_asset (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* owner = get_engine_object (ctx, this_val);
    if (!owner) return JS_EXCEPTION;
    if (!owner->getEngine ().isEvaluatingModuleTopLevel ())
        return JS_ThrowTypeError (ctx, "registerAsset can only be called from global scope");
    if (argc < 1 || !JS_IsString (argv[0]))
        return JS_ThrowTypeError (ctx, "registerAsset expects an asset path string");
    const bool precache = argc > 1 && JS_ToBool (ctx, argv[1]);
    size_t length = 0;
    const char* chars = JS_ToCStringLen (ctx, &length, argv[0]);
    if (!chars) return JS_EXCEPTION;
    const std::string path (chars, length);
    JS_FreeCString (ctx, chars);
    if (path.empty () || path.find ('\0') != std::string::npos)
        return JS_ThrowTypeError (ctx, "registerAsset requires a project-relative asset path");
    const std::filesystem::path parsed (path);
    bool parentComponent = false;
    for (const auto& component : parsed) parentComponent |= component == "..";
    if (parsed.is_absolute () || parentComponent || parsed.lexically_normal () != parsed ||
        path.find ('\\') != std::string::npos)
        return JS_ThrowTypeError (ctx, "registerAsset requires a project-relative asset path");
    if (precache) {
        try {
            // Request the asset through the loaded project container now.
            // Full native GPU/material precaching remains a separate contract.
            owner->getScene ().getScene ().project.assetLocator->read (path);
        } catch (const std::exception& error) {
            return JS_ThrowTypeError (ctx, "registerAsset cannot resolve %s: %s", path.c_str (), error.what ());
        }
    }
    JSValue handle = JS_NewObjectClass (ctx, owner->getAssetHandleClassId ());
    if (JS_IsException (handle)) return handle;
    JS_SetOpaque (handle, new RegisteredAsset {owner, path, precache});
    return handle;
}

JSValue engine_register_audio_buffers (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* owner = get_engine_object (ctx, this_val);
    if (!owner) return JS_EXCEPTION;
    if (!owner->getEngine ().isEvaluatingModuleTopLevel ())
        return JS_ThrowTypeError (ctx, "registerAudioBuffers can only be called from global scope.");
    uint32_t resolution = 16; // Native scenescript64 defaults an omitted argument to 16.
    if (argc > 0 && !JS_IsUndefined (argv[0]) && JS_ToUint32 (ctx, &resolution, argv[0]) < 0)
        return JS_EXCEPTION;
    if (resolution != 16 && resolution != 32 && resolution != 64)
        return JS_ThrowRangeError (ctx, "Resolution must be either 16, 32 or 64.");
    return owner->registerAudioBuffers (resolution);
}

EngineObject::EngineObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene) :
    m_scene (scene), m_engine (engine), m_timers (engine.getContext (), engine),
    m_classId (0), m_assetHandleClassId (0) {
    this->m_definition = { .class_name = "IEngine" };
    m_classId = registerSceneScriptClass (this->m_engine.getRuntime (), m_definition);
    m_assetHandleDefinition = {.class_name = "IAssetHandle", .finalizer = asset_handle_finalizer};
    m_assetHandleClassId = registerSceneScriptClass (this->m_engine.getRuntime (), m_assetHandleDefinition);
    this->m_instance = JS_NewObjectClass (this->m_engine.getContext (), this->m_classId);

    JS_DupValue (this->m_engine.getContext (), this->m_instance);

    // set properties
    JS_SetOpaque (this->m_instance, this);
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "frametime"),
	JS_NewCFunction (this->m_engine.getContext (), engine_get_frametime, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "runtime"),
	JS_NewCFunction (this->m_engine.getContext (), engine_get_runtime, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "timeOfDay"),
	JS_NewCFunction (this->m_engine.getContext (), engine_get_daytime, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
        this->m_engine.getContext (), this->m_instance,
        JS_NewAtom (this->m_engine.getContext (), "canvasSize"),
        JS_NewCFunction (this->m_engine.getContext (), engine_get_canvas_size, "get", 0),
        JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
        this->m_engine.getContext (), this->m_instance,
        JS_NewAtom (this->m_engine.getContext (), "screenResolution"),
        JS_NewCFunction (this->m_engine.getContext (), engine_get_screen_resolution, "get", 0),
        JS_NewCFunction (this->m_engine.getContext (), engine_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "AUDIO_RESOLUTION_16",
	JS_NewInt32 (this->m_engine.getContext (), 16), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "AUDIO_RESOLUTION_32",
	JS_NewInt32 (this->m_engine.getContext (), 32), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "AUDIO_RESOLUTION_64",
	JS_NewInt32 (this->m_engine.getContext (), 64), JS_PROP_ENUMERABLE
    );
    this->m_timers.install (this->m_instance);
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "openUserShortcut",
	JS_NewCFunction (this->m_engine.getContext (), engine_open_user_shortcut, "openUserShortcut", 0),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "registerAsset",
        JS_NewCFunction (this->m_engine.getContext (), engine_register_asset, "registerAsset", 1),
        JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
        this->m_engine.getContext (), this->m_instance, "registerAudioBuffers",
        JS_NewCFunction (this->m_engine.getContext (), engine_register_audio_buffers,
                         "registerAudioBuffers", 1), JS_PROP_ENUMERABLE
    );
    // TODO: ADD THE REST OF THE DEFINITION!
}

EngineObject::~EngineObject () {
    for (const auto& registration : m_audioBuffers)
        for (JSValue array : registration.arrays)
            JS_FreeValue (this->m_engine.getContext (), array);
    JS_FreeValue (this->m_engine.getContext (), this->m_instance);
}

JSValue EngineObject::registerAudioBuffers (uint32_t resolution) {
    JSContext* ctx = m_engine.getContext ();
    JSValue result = JS_NewObject (ctx);
    if (JS_IsException (result)) return result;
    AudioBuffers registration {resolution, {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED}};
    constexpr std::array<const char*, 3> names {"left", "right", "average"};
    for (size_t channel = 0; channel < names.size (); ++channel) {
        JSValue length = JS_NewInt32 (ctx, static_cast<int32_t> (resolution));
        JSValue array = JS_NewTypedArray (ctx, 1, &length, JS_TYPED_ARRAY_FLOAT32);
        JS_FreeValue (ctx, length);
        if (JS_IsException (array)) {
            for (JSValue stored : registration.arrays) JS_FreeValue (ctx, stored);
            JS_FreeValue (ctx, result);
            return array;
        }
        registration.arrays[channel] = JS_DupValue (ctx, array);
        if (JS_DefinePropertyValueStr (ctx, result, names[channel], array,
                                       JS_PROP_ENUMERABLE) < 0) {
            for (JSValue stored : registration.arrays) JS_FreeValue (ctx, stored);
            JS_FreeValue (ctx, result);
            return JS_EXCEPTION;
        }
    }
    m_audioBuffers.push_back (registration);
    return result;
}

void EngineObject::tick () {
    const auto& spectrum = m_scene.getAudioSpectrum ();
    JSContext* ctx = m_engine.getContext ();
    for (const auto& registration : m_audioBuffers) {
        const float* left = nullptr;
        const float* right = nullptr;
        if (registration.resolution == 16) {
            left = spectrum.audio16[0].data ();
            right = spectrum.audio16[1].data ();
        } else if (registration.resolution == 32) {
            left = spectrum.audio32[0].data ();
            right = spectrum.audio32[1].data ();
        } else {
            left = spectrum.audio64[0].data ();
            right = spectrum.audio64[1].data ();
        }
        for (size_t channel = 0; channel < 3; ++channel) {
            size_t byteOffset = 0, byteLength = 0;
            JSValue buffer = JS_GetTypedArrayBuffer (ctx, registration.arrays[channel],
                                                      &byteOffset, &byteLength, nullptr);
            if (JS_IsException (buffer)) {
                JS_FreeValue (ctx, JS_GetException (ctx));
                continue;
            }
            size_t bufferLength = 0;
            uint8_t* bytes = JS_GetArrayBuffer (ctx, &bufferLength, buffer);
            if (bytes && byteLength == registration.resolution * sizeof (float) &&
                byteOffset <= bufferLength && byteLength <= bufferLength - byteOffset) {
                float* values = reinterpret_cast<float*> (bytes + byteOffset);
                for (size_t i = 0; i < registration.resolution; ++i)
                    values[i] = channel == 0 ? left[i] : channel == 1 ? right[i]
                                 : (left[i] + right[i]) * 0.5f;
            }
            JS_FreeValue (ctx, buffer);
        }
    }
    m_timers.tick ();
}

std::optional<std::string> EngineObject::registeredAssetPath (JSValueConst value) const {
    const auto* handle = static_cast<RegisteredAsset*> (JS_GetOpaque (value, m_assetHandleClassId));
    if (!handle || handle->owner != this) return std::nullopt;
    return handle->path;
}

bool EngineObject::registeredAssetPrecached (JSValueConst value) const {
    const auto* handle = static_cast<RegisteredAsset*> (JS_GetOpaque (value, m_assetHandleClassId));
    return handle && handle->owner == this && handle->precache;
}
