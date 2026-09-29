#include "ScriptableObjectAdapter.h"

#include <utility>

#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"
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

static bool readVector (JSContext* ctx, JSValueConst value, int count, float* components);

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
    if (argc != 1 || !JS_IsNumber (argv[0]))
        return JS_ThrowTypeError (ctx, "Particle instance factor requires a number");
    double number = 0.0;
    if (JS_ToFloat64 (ctx, &number, argv[0]) < 0) return JS_EXCEPTION;
    if (!std::isfinite (number) || std::abs (number) > std::numeric_limits<float>::max ())
        return JS_ThrowTypeError (ctx, "Particle instance factor must be a finite float");
    value->update (static_cast<float> (number), DynamicValue::Script);
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

static bool particle_instance_read_vec3 (JSContext* ctx, JSValueConst value, float* components) {
    if (JS_IsNumber (value)) {
        double number = 0.0;
        if (JS_ToFloat64 (ctx, &number, value) < 0) return false;
        if (!std::isfinite (number) || std::abs (number) > std::numeric_limits<float>::max ()) {
            JS_ThrowTypeError (ctx, "Particle instance vector requires a finite float");
            return false;
        }
        components[0] = components[1] = components[2] = static_cast<float> (number);
        return true;
    }
    return readVector (ctx, value, 3, components);
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
    if (argc != 1 || !particle_instance_read_vec3 (ctx, argv[0], components)) return JS_EXCEPTION;
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
    if (argc != 1 || !particle_instance_read_vec3 (ctx, argv[0], components)) return JS_EXCEPTION;
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

JSValue scriptableobject_property_get (JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst receiver) {
    auto* container = scriptableContainer (obj_val);
    if (!container || !container->lifetime->object) return JS_ThrowTypeError (ctx, "Invalid layer object");

    const char* name = JS_AtomToCString (ctx, atom);

    if (name == nullptr) {
	return JS_EXCEPTION;
    }

    ScopeGuard guard ([=] { JS_FreeCString (ctx, name); });

    if (std::string_view (name) == "name")
        return JS_NewString (ctx, container->lifetime->object->getObject ().name.c_str ());
    if (std::string_view (name) == "getParent")
        return JS_NewCFunction (ctx, layer_get_parent, name, 0);
    if (std::string_view (name) == "getTransformMatrix")
        return JS_NewCFunction (ctx, layer_get_transform_matrix, name, 0);
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
        if (text) return JS_NewString (ctx, text->getObject ().as<Text> ()->font.c_str ());
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
    // SceneScript's public text names are lowercase, while the retained
    // renderer settings use the parser's camel-case field names.
    const std::string_view publicName (name);
    const char* propertyName = publicName == "pointsize" ? "pointSize"
        : publicName == "limitrows" ? "limitRows"
        : publicName == "maxrows" ? "maxRows"
        : publicName == "limitwidth" ? "limitWidth"
        : publicName == "maxwidth" ? "maxWidth"
        : publicName == "limituseellipsis" ? "limitUseEllipsis"
        : publicName == "opaquebackground" ? "opaqueBackground"
        : publicName == "backgroundcolor" ? "backgroundColor" : name;
    const auto it = properties.find (propertyName);
    if (it == properties.end ()) return JS_UNDEFINED;
    auto& value = it->second.value;
    const auto alive = [lifetime = std::weak_ptr (container->lifetime)] {
        const auto current = lifetime.lock ();
        return current && current->object;
    };
    const auto& adapters = container->adapter.getEngine ().getAdapters ();
    switch (value.getType ()) {
        case DynamicValue::Vec2: return adapters.vec2->instantiateLayerProperty (value, alive);
        case DynamicValue::Vec3: return adapters.vec3->instantiateLayerProperty (value, alive);
        case DynamicValue::Vec4: return adapters.vec4->instantiateLayerProperty (value, alive);
        default: return container->adapter.getEngine ().dynamicToJs (value);
    }
}

static bool readVector (JSContext* ctx, JSValueConst value, int count, float* components) {
    if (!JS_IsObject (value)) {
	JS_ThrowTypeError (ctx, "Layer vector property requires an object");
	return false;
    }
    for (int index = 0; index < count; ++index) {
	const char name[] = {"xyzw"[index], '\0'};
	JSValue component = JS_GetPropertyStr (ctx, value, name);
	if (JS_IsException (component)) return false;
	double number = 0.0;
	const bool valid = JS_IsNumber (component) && JS_ToFloat64 (ctx, &number, component) == 0
		&& std::isfinite (number) && std::abs (number) <= std::numeric_limits<float>::max ();
	JS_FreeValue (ctx, component);
	if (!valid) {
	    JS_ThrowTypeError (ctx, "Layer vector property requires finite numeric components");
	    return false;
	}
	components[index] = static_cast<float> (number);
    }
    return true;
}

int scriptableobject_property_set (
    JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst val, JSValueConst receiver, int flags
) {
    auto* container = scriptableContainer (obj_val);
    if (!container || !container->lifetime->object) return JS_ThrowTypeError (ctx, "Invalid layer object"), -1;

    const char* name = JS_AtomToCString (ctx, atom);

    if (name == nullptr) {
	return -1;
    }

    ScopeGuard guard ([=] { JS_FreeCString (ctx, name); });
    if (std::string_view (name) == "horizontalalign" ||
        std::string_view (name) == "verticalalign") {
        auto* text = dynamic_cast<WallpaperEngine::Render::Objects::CText*> (
            container->lifetime->object);
        if (!text || !JS_IsString (val))
            return JS_ThrowTypeError (ctx, "Text alignment requires a string"), -1;
        const char* value = JS_ToCString (ctx, val);
        if (!value) return -1;
        if (std::string_view (name) == "horizontalalign") text->setHorizontalAlign (value);
        else text->setVerticalAlign (value);
        JS_FreeCString (ctx, value);
        return 1;
    }
    if (std::string_view (name) == "alignment") {
        auto* image = dynamic_cast<WallpaperEngine::Render::Objects::CImage*> (
            container->lifetime->object);
        if (!image || !JS_IsString (val))
            return JS_ThrowTypeError (ctx, "Image alignment requires a string"), -1;
        const char* value = JS_ToCString (ctx, val);
        if (!value) return -1;
        image->setAlignment (value);
        JS_FreeCString (ctx, value);
        return 1;
    }
    const auto& properties = container->lifetime->object->getProperties ();
    const std::string_view publicName (name);
    const char* propertyName = publicName == "pointsize" ? "pointSize"
        : publicName == "limitrows" ? "limitRows"
        : publicName == "maxrows" ? "maxRows"
        : publicName == "limitwidth" ? "limitWidth"
        : publicName == "maxwidth" ? "maxWidth"
        : publicName == "limituseellipsis" ? "limitUseEllipsis"
        : publicName == "opaquebackground" ? "opaqueBackground"
        : publicName == "backgroundcolor" ? "backgroundColor" : name;
    const auto it = properties.find (propertyName);
    if (it == properties.end ()) return JS_ThrowTypeError (ctx, "Unknown layer property: %s", name), -1;

    auto& property = it->second.value;
    switch (property.getType ()) {
	case DynamicValue::Boolean:
	    if (!JS_IsBool (val)) break;
	    property.update (static_cast<bool> (JS_VALUE_GET_BOOL (val)), DynamicValue::Script);
	    return 1;
	case DynamicValue::Int:
	    if (!JS_IsNumber (val)) break;
	    double integer;
	    if (JS_ToFloat64 (ctx, &integer, val) < 0) return -1;
	    if (!std::isfinite (integer) || std::trunc (integer) != integer
		|| integer < std::numeric_limits<int32_t>::min ()
		|| integer > std::numeric_limits<int32_t>::max ()) break;
	    property.update (static_cast<int32_t> (integer), DynamicValue::Script);
	    return 1;
	case DynamicValue::Float:
	    if (!JS_IsNumber (val)) break;
	    double number;
	    if (JS_ToFloat64 (ctx, &number, val) < 0) return -1;
	    if (!std::isfinite (number) || std::abs (number) > std::numeric_limits<float>::max ()) break;
	    property.update (static_cast<float> (number), DynamicValue::Script);
	    return 1;
	case DynamicValue::String:
	    if (!JS_IsString (val)) break;
	    {
		size_t length = 0;
		const char* string = JS_ToCStringLen (ctx, &length, val);
		if (!string) return -1;
		property.update (std::string (string, length), DynamicValue::Script);
		JS_FreeCString (ctx, string);
		return 1;
	    }
	case DynamicValue::Vec2:
	case DynamicValue::Vec3:
	case DynamicValue::Vec4:
	    {
		float components[4] {};
		const int count = property.getType () == DynamicValue::Vec2 ? 2 : property.getType () == DynamicValue::Vec3 ? 3 : 4;
		if (!readVector (ctx, val, count, components)) return -1;
		if (count == 2) property.update (glm::vec2 (components[0], components[1]), DynamicValue::Script);
		else if (count == 3) property.update (glm::vec3 (components[0], components[1], components[2]), DynamicValue::Script);
		else property.update (glm::vec4 (components[0], components[1], components[2], components[3]), DynamicValue::Script);
		return 1;
	    }
	default:
	    break;
    }
    JS_ThrowTypeError (ctx, "Invalid value for layer property: %s", name);
    return -1;
}

ScriptableObjectAdapter::ScriptableObjectAdapter (ScriptEngine& engine, std::string name) :
    ObjectAdapter (engine), m_exoticMethods (), m_name (std::move (name)) {
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
    JSValue result = this->ObjectAdapter::instantiate (object);
    JS_SetOpaque (
	result,
	new OpaqueScriptableObjectAdapter { .magic = SCRIPTABLE_OPAQUE_MAGIC, .adapter = *this, .lifetime = object.getLifetime () }
    );

    return result;
}

JSValue ScriptableObjectAdapter::instantiate (DynamicValue& value) {
    throw std::runtime_error ("Cannot create a ScriptableObject instance from a DynamicValue");
}
