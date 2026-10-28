#include "ScriptableObjectAdapter.h"

#include <utility>

#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Scripting/LayerPropertyWrites.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"
#include "WallpaperEngine/Scripting/TextureAnimationObject.h"
#include "WallpaperEngine/Scripting/EffectThisObject.h"
#include "WallpaperEngine/Scripting/PropertyAnimationThisObject.h"
#include "WallpaperEngine/Render/Objects/CSound.h"
#include "WallpaperEngine/Render/Objects/CImage.h"
#include "WallpaperEngine/Render/Objects/CText.h"
#include "WallpaperEngine/Render/Objects/CParticle.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Render/Wallpapers/SceneTransform.h"

#include <cmath>
#include <exception>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>

using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Data::Utils;
using namespace WallpaperEngine::Scripting::Adapters;

#define SCRIPTABLE_OPAQUE_MAGIC 0xdeadbeef

// QuickJS class IDs are process-global and never reused. Check membership
// before interpreting another class's opaque payload as a layer container.
static std::mutex scriptableClassMutex;
static std::set<JSClassID> scriptableClassIds;

struct OpaqueScriptableObjectAdapter {
    unsigned int magic;
    ScriptableObjectAdapter& adapter;
    std::shared_ptr<WallpaperEngine::Scripting::ScriptableObject::Lifetime> lifetime;
};

static OpaqueScriptableObjectAdapter* scriptableContainer (JSValueConst value) {
    // Detached methods can receive any JS object as `this`; verify the class
    // before interpreting its opaque pointer as a layer container.
    const JSClassID classId = JS_GetClassID (value);
    {
        const std::lock_guard lock (scriptableClassMutex);
        if (!scriptableClassIds.contains (classId)) return nullptr;
    }
    auto* container = static_cast<OpaqueScriptableObjectAdapter*> (JS_GetOpaque (value, classId));
    return container && container->magic == SCRIPTABLE_OPAQUE_MAGIC ? container : nullptr;
}

WallpaperEngine::Scripting::ScriptableObject*
ScriptableObjectAdapter::resolve (JSValueConst value) {
    auto* container = scriptableContainer (value);
    return container && container->lifetime ? container->lifetime->object : nullptr;
}

static void scriptableobject_finalizer (JSRuntime*, JSValueConst value) {
    delete scriptableContainer (value);
}

enum SoundMethod { SoundPlay, SoundPause, SoundResume, SoundStop, SoundIsPlaying };

static JSValue sound_method (JSContext* ctx, JSValueConst thisValue, int, JSValueConst*, int method) {
    auto* container = scriptableContainer (thisValue);
    auto* sound = container && container->lifetime->object
        ? dynamic_cast<WallpaperEngine::Render::Objects::CSound*> (container->lifetime->object) : nullptr;
    if (!sound) return JS_ThrowTypeError (ctx, "Sound method requires a sound layer");

    switch (method) {
        case SoundPlay: sound->play (); break;
        case SoundPause: sound->pause (); break;
        case SoundResume: sound->resume (); break;
        case SoundStop: sound->stop (); break;
        case SoundIsPlaying: return JS_NewBool (ctx, sound->isPlaying ());
        default: return JS_ThrowInternalError (ctx, "Unknown sound method");
    }
    return JS_UNDEFINED;
}

static JSValue particle_emit_particles (JSContext* ctx, JSValueConst thisValue,
                                        int argc, JSValueConst* argv) {
    auto* container = scriptableContainer (thisValue);
    auto* particle = container && container->lifetime->object
        ? dynamic_cast<WallpaperEngine::Render::Objects::CParticle*> (container->lifetime->object)
        : nullptr;
    if (!particle) return JS_ThrowTypeError (ctx, "emitParticles requires a particle layer");
    int32_t count = 1;
    if (argc > 0 && JS_ToInt32 (ctx, &count, argv[0]) < 0) return JS_EXCEPTION;
    try {
        particle->emitParticles (count);
    } catch (const std::exception& error) {
        return JS_ThrowInternalError (ctx, "emitParticles failed: %s", error.what ());
    }
    return JS_UNDEFINED;
}

enum ParticleMethod { ParticlePlay, ParticlePause, ParticleStop, ParticleIsPlaying };

static WallpaperEngine::Render::Objects::CParticle* particle_instance_layer (JSValueConst layer) {
    return dynamic_cast<WallpaperEngine::Render::Objects::CParticle*> (
        ScriptableObjectAdapter::resolve (layer));
}

// The documented particle `instance` object reads the retained override
// UserSettings. Its accessors retain the layer wrapper, whose lifetime token
// becomes invalid when the CParticle is destroyed.
struct ParticleInstanceField {
    const char* name;
    UserSettingUniquePtr ParticleInstanceOverride::* value;
};

static constexpr ParticleInstanceField particleInstanceFields[] {
    {"alpha", &ParticleInstanceOverride::alpha},
    {"brightness", &ParticleInstanceOverride::brightness},
    {"size", &ParticleInstanceOverride::size},
    {"count", &ParticleInstanceOverride::count},
    {"speed", &ParticleInstanceOverride::speed},
    {"lifetime", &ParticleInstanceOverride::lifetime},
    {"rate", &ParticleInstanceOverride::rate},
};

static DynamicValue* particle_instance_value (JSValueConst layer, int magic) {
    if (magic < 0 || magic >= static_cast<int> (std::size (particleInstanceFields))) return nullptr;
    auto* object = ScriptableObjectAdapter::resolve (layer);
    auto* particle = dynamic_cast<WallpaperEngine::Render::Objects::CParticle*> (object);
    if (!particle) return nullptr;
    const auto& setting = particle->getParticle ().instanceOverride.*particleInstanceFields[magic].value;
    return setting && setting->value ? setting->value.get () : nullptr;
}

static JSValue particle_instance_get (JSContext* ctx, JSValueConst, int,
                                      JSValueConst*, int magic, JSValue* data) {
    auto* value = particle_instance_value (data[0], magic);
    if (!value) return JS_ThrowTypeError (ctx, "Particle instance is no longer available");
    return JS_NewFloat64 (ctx, value->getFloat ());
}

static JSValue particle_instance_set (JSContext* ctx, JSValueConst, int argc,
                                      JSValueConst* argv, int magic, JSValue* data) {
    auto* value = particle_instance_value (data[0], magic);
    if (!value) return JS_ThrowTypeError (ctx, "Particle instance is no longer available");
    // The instance factors are Float host properties and use the
    // same converter as layer properties: a non-number is skipped.
    float number = 0.0f;
    const auto result = WallpaperEngine::Scripting::convertLayerFloat (
        ctx, argc > 0 ? argv[0] : JS_UNDEFINED, number);
    if (result == WallpaperEngine::Scripting::LayerWrite::Exception) return JS_EXCEPTION;
    if (result == WallpaperEngine::Scripting::LayerWrite::Assign) value->update (number, DynamicValue::Script);
    return JS_UNDEFINED;
}

static JSValue particle_instance_vec3 (JSContext* ctx, const glm::vec3& position) {
    JSValue global = JS_GetGlobalObject (ctx);
    JSValue ctor = JS_GetPropertyStr (ctx, global, "Vec3");
    JS_FreeValue (ctx, global);
    if (JS_IsException (ctor)) return ctor;
    JSValue args[] { JS_NewFloat64 (ctx, position.x), JS_NewFloat64 (ctx, position.y),
                     JS_NewFloat64 (ctx, position.z) };
    JSValue vector = JS_CallConstructor (ctx, ctor, 3, args);
    for (auto& value : args) JS_FreeValue (ctx, value);
    JS_FreeValue (ctx, ctor);
    return vector;
}

static JSValue layer_vec2 (JSContext* ctx, const glm::vec2& size) {
    JSValue global = JS_GetGlobalObject (ctx);
    JSValue ctor = JS_GetPropertyStr (ctx, global, "Vec2");
    JS_FreeValue (ctx, global);
    if (JS_IsException (ctor)) return ctor;
    JSValue args[] {JS_NewFloat64 (ctx, size.x), JS_NewFloat64 (ctx, size.y)};
    JSValue result = JS_CallConstructor (ctx, ctor, 2, args);
    for (auto& arg : args) JS_FreeValue (ctx, arg);
    JS_FreeValue (ctx, ctor);
    return result;
}

// Instance control points and colorn are Vec3 host properties
// without the angle flag; they use the layer vector conversion.
static WallpaperEngine::Scripting::LayerWrite particle_instance_read_vec3 (
    JSContext* ctx, int argc, JSValueConst* argv, float* components) {
    return WallpaperEngine::Scripting::convertLayerVector (
        ctx, argc > 0 ? argv[0] : JS_UNDEFINED, 3, false, components);
}

static JSValue particle_instance_cp_get (JSContext* ctx, JSValueConst, int,
                                         JSValueConst*, int magic, JSValue* data) {
    auto* particle = particle_instance_layer (data[0]);
    if (!particle)
        return JS_ThrowTypeError (ctx, "Particle instance is no longer available");
    const size_t index = static_cast<size_t> (magic % 8);
    return particle_instance_vec3 (ctx, magic < 8
        ? particle->getInstanceControlPoint (index)
        : particle->getInstanceControlPointAngle (index));
}

static JSValue particle_instance_cp_set (JSContext* ctx, JSValueConst, int argc,
                                         JSValueConst* argv, int magic, JSValue* data) {
    auto* particle = particle_instance_layer (data[0]);
    if (!particle) return JS_ThrowTypeError (ctx, "Particle instance is no longer available");
    float components[3] {};
    const auto result = particle_instance_read_vec3 (ctx, argc, argv, components);
    if (result == WallpaperEngine::Scripting::LayerWrite::Exception) return JS_EXCEPTION;
    if (result == WallpaperEngine::Scripting::LayerWrite::Ignore) return JS_UNDEFINED;
    const glm::vec3 value (components[0], components[1], components[2]);
    const size_t index = static_cast<size_t> (magic % 8);
    if (magic < 8) particle->setInstanceControlPoint (index, value);
    else particle->setInstanceControlPointAngle (index, value);
    return JS_UNDEFINED;
}

static DynamicValue* particle_instance_colorn_value (JSValueConst layer) {
    auto* particle = particle_instance_layer (layer);
    if (!particle) return nullptr;
    const auto& setting = particle->getParticle ().instanceOverride.colorn;
    return setting && setting->value ? setting->value.get () : nullptr;
}

static JSValue particle_instance_colorn_get (JSContext* ctx, JSValueConst, int,
                                             JSValueConst*, int, JSValue* data) {
    auto* value = particle_instance_colorn_value (data[0]);
    if (!value) return JS_ThrowTypeError (ctx, "Particle instance is no longer available");
    return particle_instance_vec3 (ctx, value->getVec3 ());
}

static JSValue particle_instance_colorn_set (JSContext* ctx, JSValueConst, int argc,
                                             JSValueConst* argv, int, JSValue* data) {
    auto* value = particle_instance_colorn_value (data[0]);
    if (!value) return JS_ThrowTypeError (ctx, "Particle instance is no longer available");
    float components[3] {};
    const auto result = particle_instance_read_vec3 (ctx, argc, argv, components);
    if (result == WallpaperEngine::Scripting::LayerWrite::Exception) return JS_EXCEPTION;
    if (result == WallpaperEngine::Scripting::LayerWrite::Ignore) return JS_UNDEFINED;
    value->update (glm::vec3 (components[0], components[1], components[2]), DynamicValue::Script);
    return JS_UNDEFINED;
}

static JSValue particle_instance_object (JSContext* ctx, JSValueConst layer) {
    JSValue instance = JS_NewObject (ctx);
    if (JS_IsException (instance)) return instance;
    for (int i = 0; i < static_cast<int> (std::size (particleInstanceFields)); ++i) {
        JSValue data[] { JS_DupValue (ctx, layer) };
        JSValue getter = JS_NewCFunctionData (ctx, particle_instance_get, 0, i, 1, data);
        JSValue setter = JS_NewCFunctionData (ctx, particle_instance_set, 1, i, 1, data);
        JS_FreeValue (ctx, data[0]);
        JSAtom atom = JS_NewAtom (ctx, particleInstanceFields[i].name);
        const int result = JS_DefinePropertyGetSet (
            ctx, instance, atom, getter, setter, JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
        JS_FreeAtom (ctx, atom);
        if (result < 0) {
            JS_FreeValue (ctx, instance);
            return JS_EXCEPTION;
        }
    }
    for (int i = 0; i < 16; ++i) {
        const std::string name = (i < 8 ? "controlpoint" : "controlpointangle")
            + std::to_string (i % 8);
        JSValue data[] { JS_DupValue (ctx, layer) };
        JSValue getter = JS_NewCFunctionData (ctx, particle_instance_cp_get, 0, i, 1, data);
        JSValue setter = JS_NewCFunctionData (ctx, particle_instance_cp_set, 1, i, 1, data);
        JS_FreeValue (ctx, data[0]);
        JSAtom atom = JS_NewAtom (ctx, name.c_str ());
        const int result = JS_DefinePropertyGetSet (
            ctx, instance, atom, getter, setter, JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
        JS_FreeAtom (ctx, atom);
        if (result < 0) { JS_FreeValue (ctx, instance); return JS_EXCEPTION; }
    }
    {
        JSValue data[] { JS_DupValue (ctx, layer) };
        JSValue getter = JS_NewCFunctionData (ctx, particle_instance_colorn_get, 0, 0, 1, data);
        JSValue setter = JS_NewCFunctionData (ctx, particle_instance_colorn_set, 1, 0, 1, data);
        JS_FreeValue (ctx, data[0]);
        JSAtom atom = JS_NewAtom (ctx, "colorn");
        const int result = JS_DefinePropertyGetSet (
            ctx, instance, atom, getter, setter, JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
        JS_FreeAtom (ctx, atom);
        if (result < 0) { JS_FreeValue (ctx, instance); return JS_EXCEPTION; }
    }
    return instance;
}

static JSValue particle_method (JSContext* ctx, JSValueConst thisValue, int,
                                JSValueConst*, int method) {
    auto* container = scriptableContainer (thisValue);
    auto* particle = container && container->lifetime->object
        ? dynamic_cast<WallpaperEngine::Render::Objects::CParticle*> (container->lifetime->object)
        : nullptr;
    if (!particle) return JS_ThrowTypeError (ctx, "Particle method requires a particle layer");
    switch (method) {
        case ParticlePlay: particle->play (); break;
        case ParticlePause: particle->pause (); break;
        case ParticleStop: particle->stop (); break;
        case ParticleIsPlaying: return JS_NewBool (ctx, particle->isPlaying ());
        default: return JS_ThrowInternalError (ctx, "Unknown particle method");
    }
    return JS_UNDEFINED;
}

static JSValue layer_material_function (JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv) {
    auto* container = scriptableContainer (thisValue);
    auto* layer = container && container->lifetime->object ? container->lifetime->object : nullptr;
    auto* image = dynamic_cast<WallpaperEngine::Render::Objects::CImage*> (layer);
    auto* text = dynamic_cast<WallpaperEngine::Render::Objects::CText*> (layer);
    if (!image && !text) return JS_ThrowTypeError (ctx, "Material function requires an image or text layer");
    if (argc < 1 || !JS_IsString (argv[0]))
        return JS_ThrowTypeError (ctx, "Material function name must be a string");
    const char* name = JS_ToCString (ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    const std::string functionName (name);
    JS_FreeCString (ctx, name);
    try {
        if (image) image->executeMaterialFunction (functionName);
        else text->executeMaterialFunction (functionName);
    } catch (const std::exception& error) {
        return JS_ThrowInternalError (ctx, "Material function failed: %s", error.what ());
    }
    return JS_UNDEFINED;
}

static JSValue layer_get_parent (JSContext* ctx, JSValueConst thisValue, int, JSValueConst*) {
    auto* container = scriptableContainer (thisValue);
    auto* layer = container && container->lifetime ? container->lifetime->object : nullptr;
    if (!layer) return JS_ThrowTypeError (ctx, "getParent requires a live layer");
    const auto parentId = layer->getObject ().parent;
    if (!parentId) return JS_UNDEFINED;
    const auto* parent = layer->getScene ().getObject (*parentId);
    const auto* scriptable = parent ? dynamic_cast<const WallpaperEngine::Scripting::ScriptableObject*> (parent)
                                    : nullptr;
    return scriptable ? container->adapter.getEngine ().getAdapters ().object->instantiate (
                            const_cast<WallpaperEngine::Scripting::ScriptableObject&> (*scriptable))
                      : JS_UNDEFINED;
}

static JSValue layer_get_texture_animation (JSContext* ctx, JSValueConst thisValue,
                                           int, JSValueConst*) {
    auto* container = scriptableContainer (thisValue);
    auto* layer = container && container->lifetime ? container->lifetime->object : nullptr;
    auto* image = dynamic_cast<WallpaperEngine::Render::Objects::CImage*> (layer);
    if (!image) return JS_ThrowTypeError (ctx, "getTextureAnimation requires an image layer");
    return container->adapter.getEngine ().getAdapters ().textureAnimation->instantiate (
        layer, image->getTextureAnimation ());
}

// Native 2.8.42 IEffectLayer getEffect, on image and text
// layers: an integer selects an effect by index; a non-empty string selects
// the last effect with exactly that name. Any other argument (including a
// fractional or NaN number), or no match, returns null.
static JSValue layer_get_effect (JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv) {
    auto* container = scriptableContainer (thisValue);
    auto* layer = container && container->lifetime ? container->lifetime->object : nullptr;
    const auto* effects = WallpaperEngine::Scripting::layerEffects (layer);
    if (!effects) return JS_ThrowTypeError (ctx, "getEffect requires an image or text layer");
    std::optional<size_t> index;
    if (argc > 0 && JS_IsNumber (argv[0])) {
        double requested = 0.0;
        if (JS_ToFloat64 (ctx, &requested, argv[0]) < 0) return JS_EXCEPTION;
        if (std::isfinite (requested) && std::trunc (requested) == requested && requested >= 0.0
            && requested < static_cast<double> (effects->size ()))
            index = static_cast<size_t> (requested);
    } else if (argc > 0 && JS_IsString (argv[0])) {
        size_t length = 0;
        const char* chars = JS_ToCStringLen (ctx, &length, argv[0]);
        if (!chars) return JS_EXCEPTION;
        const std::string_view requested (chars, length);
        if (!requested.empty ())
            for (size_t i = 0; i < effects->size (); ++i)
                if ((*effects)[i] && (*effects)[i]->name == requested) index = i;
        JS_FreeCString (ctx, chars);
    }
    if (!index) return JS_NULL;
    return WallpaperEngine::Scripting::createEffectThisObject (ctx, thisValue, *index);
}

static JSValue layer_get_effect_count (JSContext* ctx, JSValueConst thisValue, int, JSValueConst*) {
    auto* container = scriptableContainer (thisValue);
    auto* layer = container && container->lifetime ? container->lifetime->object : nullptr;
    const auto* effects = WallpaperEngine::Scripting::layerEffects (layer);
    if (!effects) return JS_ThrowTypeError (ctx, "getEffectCount requires an image or text layer");
    return JS_NewInt64 (ctx, static_cast<int64_t> (effects->size ()));
}

// Native IObject.getAnimation: a non-empty
// name selects this object's timeline animation with that name. Without
// one, the running script's own property animation name is used. A missing
// animation is undefined.
static JSValue layer_get_animation (JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv) {
    auto* container = scriptableContainer (thisValue);
    auto* layer = container && container->lifetime ? container->lifetime->object : nullptr;
    if (!layer) return JS_UNDEFINED;
    std::string name;
    if (argc > 0 && JS_IsString (argv[0])) {
        size_t length = 0;
        const char* chars = JS_ToCStringLen (ctx, &length, argv[0]);
        if (!chars) return JS_EXCEPTION;
        name.assign (chars, length);
        JS_FreeCString (ctx, chars);
    }
    if (name.empty ()) {
        // Timer callbacks keep the scheduling script's property as their context.
        const auto running = container->adapter.getEngine ().currentScriptProperty ();
        if (!running.value || !running.value->getAnimation ()) return JS_UNDEFINED;
        if (running.object == layer)
            return WallpaperEngine::Scripting::createPropertyAnimationObject (ctx, thisValue, *running.value);
        name = running.value->getAnimation ()->name;
    }
    auto* value = WallpaperEngine::Scripting::findObjectAnimation (*layer, name);
    if (!value) return JS_UNDEFINED;
    return WallpaperEngine::Scripting::createPropertyAnimationObject (ctx, thisValue, *value);
}

// Native SceneScript Mat4 stores its column-major elements in `m`; authored
// scripts read indices 12 and 13 for the layer's current world translation.
// Use the same resolved hierarchy as scene rendering, including attachments,
// rather than the layer's local origin or a screen-projected matrix.
static JSValue layer_get_transform_matrix (JSContext* ctx, JSValueConst thisValue,
                                            int, JSValueConst*) {
    auto* container = scriptableContainer (thisValue);
    auto* layer = container && container->lifetime ? container->lifetime->object : nullptr;
    if (!layer) return JS_ThrowTypeError (ctx, "getTransformMatrix requires a live layer");
    glm::mat4 matrix;
    try {
        auto& scene = layer->getScene ();
        matrix = WallpaperEngine::Render::Wallpapers::resolveSceneTransform (
            layer->getObject (), [&scene] (int parentId) -> const Object* {
                const auto* parent = scene.getObject (parentId);
                return parent ? &parent->getObject () : nullptr;
            }, [&scene] (const Object& parent, const std::string& name) {
                return scene.getPuppetAttachmentTransform (parent.id, name);
            }).authoredMatrix;
    } catch (const std::exception& error) {
        return JS_ThrowTypeError (ctx, "getTransformMatrix failed: %s", error.what ());
    }
    JSValue elements = JS_NewArray (ctx);
    if (JS_IsException (elements)) return elements;
    for (uint32_t column = 0; column < 4; ++column)
        for (uint32_t row = 0; row < 4; ++row)
            JS_SetPropertyUint32 (ctx, elements, column * 4 + row,
                                  JS_NewFloat64 (ctx, matrix[column][row]));
    JSValue result = JS_NewObject (ctx);
    if (JS_IsException (result)) { JS_FreeValue (ctx, elements); return result; }
    JS_SetPropertyStr (ctx, result, "m", elements);
    return result;
}

// Native ILayer.rotateObjectSpace takes one Vec3.
// native reads an object argument into a zeroed Vec3, replacing only
// the numeric x, y and z; the components are radians.
// What native does with a non-object argument is not recovered, so Linux
// leaves the layer unchanged.
static JSValue layer_rotate_object_space (JSContext* ctx, JSValueConst thisValue,
                                          int argc, JSValueConst* argv) {
    auto* container = scriptableContainer (thisValue);
    auto* layer = container && container->lifetime ? container->lifetime->object : nullptr;
    if (!layer) return JS_ThrowTypeError (ctx, "rotateObjectSpace requires a live layer");
    if (argc < 1 || !JS_IsObject (argv[0])) return JS_UNDEFINED;
    glm::vec3 rotation (0.0f);
    for (int component = 0; component < 3; ++component) {
        const char name[] = {"xyz"[component], '\0'};
        JSValue part = JS_GetPropertyStr (ctx, argv[0], name);
        if (JS_IsException (part)) return part;
        if (JS_IsNumber (part)) {
            double number = 0.0;
            if (JS_ToFloat64 (ctx, &number, part) < 0) {
                JS_FreeValue (ctx, part);
                return JS_EXCEPTION;
            }
            rotation[component] = static_cast<float> (number);
        }
        JS_FreeValue (ctx, part);
    }
    const auto& properties = layer->getProperties ();
    const auto it = properties.find ("angles");
    if (it == properties.end ()) return JS_UNDEFINED;
    auto& angles = it->second.value;
    angles.update (WallpaperEngine::Render::Wallpapers::rotateObjectSpaceAngles (angles.getVec3 (), rotation),
                   DynamicValue::Script);
    return JS_UNDEFINED;
}

JSValue scriptableobject_property_get (JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst receiver) {
    auto* container = scriptableContainer (obj_val);
    if (!container) return JS_ThrowTypeError (ctx, "Invalid layer object");
    // Native retained handles become inert when their layer is removed.
    if (!container->lifetime->object) return JS_UNDEFINED;

    const char* name = JS_AtomToCString (ctx, atom);

    if (name == nullptr) {
	return JS_EXCEPTION;
    }

    ScopeGuard guard ([=] { JS_FreeCString (ctx, name); });

    if (std::string_view (name) == "name") {
        const auto& layerName = container->lifetime->object->getScriptName ();
        return JS_NewStringLen (ctx, layerName.data (), layerName.size ());
    }
    if (std::string_view (name) == "solid")
        return JS_NewBool (ctx, container->lifetime->object->isSolid ());
    if (std::string_view (name) == "disablepropagation")
        return JS_NewBool (ctx, container->lifetime->object->disablesCursorPropagation ());
    if (std::string_view (name) == "getParent")
        return JS_NewCFunction (ctx, layer_get_parent, name, 0);
    if (std::string_view (name) == "getTransformMatrix")
        return JS_NewCFunction (ctx, layer_get_transform_matrix, name, 0);
    if (std::string_view (name) == "rotateObjectSpace")
        return JS_NewCFunction (ctx, layer_rotate_object_space, name, 1);
    if (std::string_view (name) == "getTextureAnimation"
        && dynamic_cast<WallpaperEngine::Render::Objects::CImage*> (container->lifetime->object))
        return JS_NewCFunction (ctx, layer_get_texture_animation, name, 0);
    if (std::string_view (name) == "getAnimation")
        return JS_NewCFunction (ctx, layer_get_animation, name, 1);
    if (WallpaperEngine::Scripting::layerEffects (container->lifetime->object)) {
        if (std::string_view (name) == "getEffect") return JS_NewCFunction (ctx, layer_get_effect, name, 1);
        if (std::string_view (name) == "getEffectCount")
            return JS_NewCFunction (ctx, layer_get_effect_count, name, 0);
    }
    if (auto* text = dynamic_cast<WallpaperEngine::Render::Objects::CText*> (
            container->lifetime->object)) {
        const std::string_view field (name);
        if (field == "horizontalalign") return JS_NewString (ctx, text->getHorizontalAlign ().c_str ());
        if (field == "verticalalign") return JS_NewString (ctx, text->getVerticalAlign ().c_str ());
        if (field == "size") return layer_vec2 (ctx, text->getLayoutSize ());
    }
    if (std::string_view (name) == "alignment") {
        if (const auto* image = dynamic_cast<const WallpaperEngine::Render::Objects::CImage*> (
                container->lifetime->object))
            return JS_NewString (ctx, image->getAlignment ().c_str ());
    }
    if (std::string_view (name) == "font") {
        const auto* text = dynamic_cast<WallpaperEngine::Render::Objects::CText*> (
            container->lifetime->object);
        if (text) return JS_NewStringLen (ctx, text->getFont ().data (), text->getFont ().size ());
    }

    if ((dynamic_cast<WallpaperEngine::Render::Objects::CImage*> (container->lifetime->object)
         || dynamic_cast<WallpaperEngine::Render::Objects::CText*> (container->lifetime->object))
        && std::string_view (name) == "executeMaterialFunction")
        return JS_NewCFunction (ctx, layer_material_function, name, 1);

    if (dynamic_cast<WallpaperEngine::Render::Objects::CParticle*> (
            container->lifetime->object)) {
        const std::string_view methodName (name);
        if (methodName == "instance") return particle_instance_object (ctx, obj_val);
        if (methodName == "emitParticles")
            return JS_NewCFunction (ctx, particle_emit_particles, name, 1);
        const auto method = methodName == "play" ? ParticlePlay
            : methodName == "pause" ? ParticlePause
            : methodName == "stop" ? ParticleStop
            : methodName == "isPlaying" ? ParticleIsPlaying : -1;
        if (method >= 0)
            return JS_NewCFunctionMagic (ctx, particle_method, name, 0,
                                         JS_CFUNC_generic_magic, method);
    }

    if (dynamic_cast<WallpaperEngine::Render::Objects::CSound*> (container->lifetime->object)) {
        const std::string_view methodName (name);
        const auto method = methodName == "play" ? SoundPlay
            : methodName == "pause" ? SoundPause
            : methodName == "resume" ? SoundResume
            : methodName == "stop" ? SoundStop
            : methodName == "isPlaying" ? SoundIsPlaying : -1;
        if (method >= 0)
            return JS_NewCFunctionMagic (ctx, sound_method, name, 0, JS_CFUNC_generic_magic, method);
    }

    const auto& properties = container->lifetime->object->getProperties ();
    const auto key = WallpaperEngine::Scripting::layerPropertyKey (name);
    if (!key) return JS_UNDEFINED;
    const auto it = properties.find (std::string (*key));
    if (it == properties.end ()) return JS_UNDEFINED;
    auto& value = it->second.value;
    if (value.getType () == DynamicValue::Vec4
        && container->lifetime->object->isRgbColorProperty (value)) {
        DynamicValue rgb (glm::vec3 (value.getVec4 ()));
        return container->adapter.getEngine ().dynamicToJs (rgb, true);
    }
    // Native layer vector getters return detached values. Editing a retained
    // component does not set the layer; whole-property assignment does.
    return container->adapter.getEngine ().dynamicToJs (
        value, true, container->lifetime->object->isAngleProperty (value));
}

// Applies a converter result: an exception propagates, and a skipped value
// leaves the property unchanged.
static int layerWriteResult (WallpaperEngine::Scripting::LayerWrite result) {
    return result == WallpaperEngine::Scripting::LayerWrite::Exception ? -1 : 1;
}

int scriptableobject_property_set (
    JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst val, JSValueConst receiver, int flags
) {
    using WallpaperEngine::Scripting::LayerWrite;
    auto* container = scriptableContainer (obj_val);
    if (!container) return JS_ThrowTypeError (ctx, "Invalid layer object"), -1;
    if (!container->lifetime->object) return 1;
    auto* layer = container->lifetime->object;

    const char* name = JS_AtomToCString (ctx, atom);

    if (name == nullptr) {
	return -1;
    }

    ScopeGuard guard ([=] { JS_FreeCString (ctx, name); });
    const std::string_view publicName (name);
    // Native layers have no read-only host properties: every host property
    // shares the setter with attribute 0. Each write below goes
    // through the type converter.
    if (publicName == "solid" || publicName == "disablepropagation") {
        // These flags affect live input routing without changing the authored config.
        bool flag = false;
        if (WallpaperEngine::Scripting::convertLayerBoolean (val, flag) == LayerWrite::Assign) {
            if (publicName == "solid") layer->setSolid (flag);
            else layer->setDisablePropagation (flag);
        }
        return 1;
    }
    auto* text = dynamic_cast<WallpaperEngine::Render::Objects::CText*> (layer);
    auto* image = dynamic_cast<WallpaperEngine::Render::Objects::CImage*> (layer);
    // String host properties. `name` is on every layer; text layers add the
    // alignments and `font`; image layers add `alignment`. On other layers
    // these names fall through to the unknown-name path below.
    const bool textString = text && (publicName == "horizontalalign" || publicName == "verticalalign"
                                     || publicName == "font");
    if (publicName == "name" || textString || (image && publicName == "alignment")) {
        std::string value;
        const auto result = WallpaperEngine::Scripting::convertLayerString (ctx, val, value);
        if (result != LayerWrite::Assign) return layerWriteResult (result);
        if (publicName == "name") layer->setScriptName (std::move (value));
        else if (publicName == "alignment") image->setAlignment (value);
        else if (publicName == "horizontalalign") text->setHorizontalAlign (value);
        else if (publicName == "verticalalign") text->setVerticalAlign (value);
        else text->setFont (std::move (value));
        return 1;
    }
    // Text inherits the image `size` host property (a Vec2 with no change callback);
    // the text lookup falls back to
    // the image map). How text layout uses that field after a write is not
    // recovered, so the write is accepted without effect.
    if (text && publicName == "size") return 1;
    // The particle `instance` host property is registered as type 7
    // and the converter's case 7 returns without calling the
    // setter.
    if (publicName == "instance" && dynamic_cast<WallpaperEngine::Render::Objects::CParticle*> (layer))
        return 1;

    const auto& properties = layer->getProperties ();
    const auto key = WallpaperEngine::Scripting::layerPropertyKey (publicName);
    const auto it = key ? properties.find (std::string (*key)) : properties.end ();
    if (it == properties.end ()) {
        // Any other name is not a host property. A method is a writable data
        // member of the native template. Either way the write is stored on
        // the receiver, so later reads see it.
        return WallpaperEngine::Scripting::defineLayerExpando (ctx, receiver, atom, val, flags);
    }

    auto& property = it->second.value;
    switch (property.getType ()) {
	case DynamicValue::Boolean: {
	    bool value = false;
	    const auto result = WallpaperEngine::Scripting::convertLayerBoolean (val, value);
	    if (result == LayerWrite::Assign) property.update (value, DynamicValue::Script);
	    return layerWriteResult (result);
	}
	case DynamicValue::Int: {
	    int32_t value = 0;
	    const auto result = WallpaperEngine::Scripting::convertLayerInt (ctx, val, value);
	    if (result == LayerWrite::Assign) property.update (value, DynamicValue::Script);
	    return layerWriteResult (result);
	}
	case DynamicValue::Float: {
	    float value = 0.0f;
	    const auto result = WallpaperEngine::Scripting::convertLayerFloat (ctx, val, value);
	    if (result == LayerWrite::Assign) property.update (value, DynamicValue::Script);
	    return layerWriteResult (result);
	}
	case DynamicValue::String: {
	    std::string value;
	    const auto result = WallpaperEngine::Scripting::convertLayerString (ctx, val, value);
	    if (result == LayerWrite::Assign) property.update (value, DynamicValue::Script);
	    return layerWriteResult (result);
	}
	case DynamicValue::Vec2:
	case DynamicValue::Vec3:
	case DynamicValue::Vec4: {
	    // Layer colours are Vec3 host properties kept with their alpha in a Vec4.
	    const bool rgbColor = property.getType () == DynamicValue::Vec4 && layer->isRgbColorProperty (property);
	    const int count = property.getType () == DynamicValue::Vec2 ? 2
		: property.getType () == DynamicValue::Vec3 || rgbColor ? 3 : 4;
	    const bool angle = count == 3 && layer->isAngleProperty (property);
	    float components[4] {};
	    const auto result = WallpaperEngine::Scripting::convertLayerVector (ctx, val, count, angle, components);
	    if (result != LayerWrite::Assign) return layerWriteResult (result);
	    if (rgbColor)
		property.update (glm::vec4 (components[0], components[1], components[2], property.getVec4 ().w),
				 DynamicValue::Script);
	    else if (count == 2) property.update (glm::vec2 (components[0], components[1]), DynamicValue::Script);
	    else if (count == 3)
		property.update (glm::vec3 (components[0], components[1], components[2]), DynamicValue::Script);
	    else
		property.update (glm::vec4 (components[0], components[1], components[2], components[3]),
				 DynamicValue::Script);
	    return 1;
	}
	default:
	    // No other value type is retained for a layer host property.
	    return 1;
    }
}

ScriptableObjectAdapter::ScriptableObjectAdapter (ScriptEngine& engine, std::string name) :
    ObjectAdapter (engine), m_instances (engine.getContext ()), m_exoticMethods (), m_name (std::move (name)) {
    m_exoticMethods.get_property = scriptableobject_property_get;
    m_exoticMethods.set_property = scriptableobject_property_set;
    this->registerType (
	{
	    .class_name = m_name.c_str (),
	    .finalizer = scriptableobject_finalizer,
	    .exotic = &m_exoticMethods,
	}
    );
    const std::lock_guard lock (scriptableClassMutex);
    scriptableClassIds.insert (m_classId);
}

JSValue ScriptableObjectAdapter::instantiate (ScriptableObject& object) {
    JSValue cached = m_instances.find (&object);
    if (!JS_IsUndefined (cached)) return cached;
    JSValue result = this->ObjectAdapter::instantiate (object);
    if (JS_IsException (result)) return result;
    JS_SetOpaque (
	result,
	new OpaqueScriptableObjectAdapter { .magic = SCRIPTABLE_OPAQUE_MAGIC, .adapter = *this, .lifetime = object.getLifetime () }
    );

    // The wrapper retains only the lifetime token, not the native layer.
    // Removing a layer invalidates that token and releases this cache entry.
    m_instances.retain (&object, result);
    return result;
}

void ScriptableObjectAdapter::forgetInstance (const ScriptableObject& object) {
    m_instances.forget (&object);
}

void ScriptableObjectAdapter::releaseInstances () { m_instances.clear (); }

JSValue ScriptableObjectAdapter::instantiate (DynamicValue& value) {
    throw std::runtime_error ("Cannot create a ScriptableObject instance from a DynamicValue");
}
