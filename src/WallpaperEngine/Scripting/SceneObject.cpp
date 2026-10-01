#include "SceneObject.h"

#include "Adapters/ScriptableObjectAdapter.h"
#include "ScriptEngine.h"
#include "SceneScriptClassRegistration.h"
#include "SceneCameraTransforms.h"
#include "ScriptableObject.h"
#include "WorkshopScriptAssetPath.h"
#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <array>

using namespace WallpaperEngine::Scripting;
SceneObject* get_opaque (JSContext* ctx, JSValueConst this_val);

namespace {
using DynamicModelData = WallpaperEngine::Data::Model::DynamicModelData;
using DynamicModelShape = WallpaperEngine::Data::Model::DynamicModelShape;
using ModelHandle = std::shared_ptr<DynamicModelData>;

void model_data_finalizer (JSRuntime*, JSValue value) {
    delete static_cast<ModelHandle*> (JS_GetOpaque (value, JS_GetClassID (value)));
}

JSValue model_property (JSContext* ctx, JSValueConst object, const char* name) {
    JSValue value = JS_GetPropertyStr (ctx, object, name);
    if (JS_IsException (value)) throw std::invalid_argument (std::string ("Cannot read model field ") + name);
    return value;
}

std::vector<double> model_numbers (JSContext* ctx, JSValueConst value, const char* name) {
    if (!JS_IsObject (value)) throw std::invalid_argument (std::string (name) + " must be an array");
    JSValue lengthValue = model_property (ctx, value, "length");
    uint32_t length = 0;
    const int converted = JS_ToUint32 (ctx, &length, lengthValue);
    JS_FreeValue (ctx, lengthValue);
    if (converted < 0 || length > 4000000) throw std::invalid_argument (std::string (name) + " has invalid length");
    std::vector<double> result;
    result.reserve (length);
    for (uint32_t i = 0; i < length; ++i) {
        JSValue item = JS_GetPropertyUint32 (ctx, value, i);
        double number = 0;
        const int valid = JS_ToFloat64 (ctx, &number, item);
        JS_FreeValue (ctx, item);
        if (valid < 0 || !std::isfinite (number))
            throw std::invalid_argument (std::string (name) + " contains a nonfinite number");
        result.push_back (number);
    }
    return result;
}

void model_vertices (DynamicModelShape& shape, const std::vector<double>& values) {
    int stride = 0;
    std::array<bool, 4> seen {};
    for (int format : shape.vertexFormat) {
        if (format < 0 || format > 3) throw std::invalid_argument ("Unsupported model vertex format");
        if (seen[size_t (format)]) throw std::invalid_argument ("Duplicate model vertex attribute");
        seen[size_t (format)] = true;
        stride += format == 2 ? 2 : format == 3 ? 4 : 3;
    }
    if (!seen[0] || stride == 0 || values.size () % size_t (stride) != 0 || values.size () / stride > 4000000)
        throw std::invalid_argument ("Invalid model vertex buffer length");
    for (double value : values)
        if (std::abs (value) > std::numeric_limits<float>::max ())
            throw std::invalid_argument ("Model vertex exceeds finite float range");
    shape.positions.clear ();
    shape.normals.clear ();
    shape.tangents.clear ();
    shape.texcoords.clear ();
    for (size_t offset = 0; offset < values.size (); offset += stride) {
        size_t field = offset;
        for (int format : shape.vertexFormat) {
            if (format == 0) shape.positions.push_back ({float (values[field]), float (values[field + 1]), float (values[field + 2])});
            if (format == 1) shape.normals.push_back ({float (values[field]), float (values[field + 1]), float (values[field + 2])});
            if (format == 2) shape.texcoords.push_back ({float (values[field]), float (values[field + 1])});
            if (format == 3) shape.tangents.push_back ({float (values[field]), float (values[field + 1]), float (values[field + 2]), float (values[field + 3])});
            field += format == 2 ? 2 : format == 3 ? 4 : 3;
        }
    }
    if (shape.positions.size () != values.size () / stride)
        throw std::invalid_argument ("Model vertex format requires POSITION");
}

DynamicModelShape model_shape (JSContext* ctx, ScriptEngine& engine, JSValueConst value) {
    if (!JS_IsObject (value)) throw std::invalid_argument ("Model shape must be an object");
    DynamicModelShape shape;
    JSValue format = model_property (ctx, value, "vertexFormat");
    try {
        for (double number : model_numbers (ctx, format, "vertexFormat")) {
            if (number != std::trunc (number) || number < 0 || number > 3)
                throw std::invalid_argument ("Invalid model vertex format");
            shape.vertexFormat.push_back (int (number));
        }
    } catch (...) { JS_FreeValue (ctx, format); throw; }
    JS_FreeValue (ctx, format);
    JSValue vertices = model_property (ctx, value, "vertexBuffer");
    try { model_vertices (shape, model_numbers (ctx, vertices, "vertexBuffer")); }
    catch (...) { JS_FreeValue (ctx, vertices); throw; }
    JS_FreeValue (ctx, vertices);
    JSValue indices = model_property (ctx, value, "indexBuffer");
    if (!JS_IsUndefined (indices)) {
        try {
            for (double number : model_numbers (ctx, indices, "indexBuffer")) {
                if (number < 0 || number != std::trunc (number) || number > UINT32_MAX
                    || number >= shape.positions.size ()) throw std::invalid_argument ("Invalid model index");
                shape.indices.push_back (uint32_t (number));
            }
        } catch (...) { JS_FreeValue (ctx, indices); throw; }
    }
    JS_FreeValue (ctx, indices);
    JSValue material = model_property (ctx, value, "material");
    const auto path = engine.registeredAssetPath (material);
    const bool precached = engine.registeredAssetPrecached (material);
    JS_FreeValue (ctx, material);
    if (!path) throw std::invalid_argument ("Model material requires a registered asset handle");
    if (!precached) throw std::invalid_argument ("Model material requires a precached asset handle");
    shape.material = *path;
    for (auto [name, output] : {std::pair {"isVertexBufferDynamic", &shape.vertexDynamic},
                                {"isIndexBufferDynamic", &shape.indexDynamic}}) {
        JSValue flag = model_property (ctx, value, name);
        *output = JS_ToBool (ctx, flag) > 0;
        JS_FreeValue (ctx, flag);
    }
    return shape;
}

std::vector<DynamicModelShape> model_shapes (JSContext* ctx, ScriptEngine& engine, JSValueConst configuration) {
    JSValue shapes = model_property (ctx, configuration, "shapes");
    if (!JS_IsObject (shapes)) { JS_FreeValue (ctx, shapes); throw std::invalid_argument ("Model requires shapes array"); }
    JSValue lengthValue = model_property (ctx, shapes, "length");
    uint32_t length = 0;
    const int status = JS_ToUint32 (ctx, &length, lengthValue);
    JS_FreeValue (ctx, lengthValue);
    if (status < 0 || length > 256) { JS_FreeValue (ctx, shapes); throw std::invalid_argument ("Invalid model shape count"); }
    std::vector<DynamicModelShape> result;
    try {
        for (uint32_t i = 0; i < length; ++i) {
            JSValue shape = JS_GetPropertyUint32 (ctx, shapes, i);
            try { result.push_back (model_shape (ctx, engine, shape)); }
            catch (...) { JS_FreeValue (ctx, shape); throw; }
            JS_FreeValue (ctx, shape);
        }
    } catch (...) { JS_FreeValue (ctx, shapes); throw; }
    JS_FreeValue (ctx, shapes);
    return result;
}

ModelHandle* model_handle (JSContext* ctx, JSValueConst value) {
    const auto* engine = static_cast<ScriptEngine*> (JS_GetContextOpaque (ctx));
    return engine && engine->getSceneObject ()
        ? static_cast<ModelHandle*> (JS_GetOpaque (value, engine->getSceneObject ()->getModelDataClassId ()))
        : nullptr;
}

JSValue model_replace_data (JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv) {
    auto* handle = model_handle (ctx, thisValue);
    auto* engine = static_cast<ScriptEngine*> (JS_GetContextOpaque (ctx));
    if (!handle || !engine) return JS_ThrowTypeError (ctx, "replaceData requires IModelData");
    if ((*handle)->destroyed) return JS_ThrowTypeError (ctx, "Model data has been destroyed");
    if (argc != 1 || !JS_IsObject (argv[0])) return JS_ThrowTypeError (ctx, "replaceData requires model configuration");
    try {
        auto shapes = model_shapes (ctx, *engine, argv[0]);
        (*handle)->shapes = std::move (shapes);
        ++(*handle)->revision;
        ++(*handle)->structureRevision;
        return JS_UNDEFINED;
    } catch (const std::exception& error) { return JS_ThrowTypeError (ctx, "%s", error.what ()); }
}

void model_apply_shape (JSContext* ctx, DynamicModelShape& shape, JSValueConst update) {
    if (!JS_IsObject (update)) throw std::invalid_argument ("Model shape update must be an object");
    JSValue vertices = model_property (ctx, update, "vertexBuffer");
    if (!JS_IsUndefined (vertices)) {
        try {
            if (!shape.vertexDynamic) throw std::invalid_argument ("Model vertex buffer is not dynamic");
            const size_t previous = shape.positions.size ();
            model_vertices (shape, model_numbers (ctx, vertices, "vertexBuffer"));
            if (shape.positions.size () != previous)
                throw std::invalid_argument ("applyData cannot resize the vertex buffer");
        } catch (...) { JS_FreeValue (ctx, vertices); throw; }
    }
    JS_FreeValue (ctx, vertices);
    JSValue indices = model_property (ctx, update, "indexBuffer");
    if (!JS_IsUndefined (indices)) {
        try {
            if (!shape.indexDynamic) throw std::invalid_argument ("Model index buffer is not dynamic");
            const auto values = model_numbers (ctx, indices, "indexBuffer");
            if (values.size () != shape.indices.size ())
                throw std::invalid_argument ("applyData cannot resize the index buffer");
            std::vector<uint32_t> replacement;
            replacement.reserve (values.size ());
            for (double number : values) {
                if (number < 0 || number != std::trunc (number) || number > UINT32_MAX
                    || number >= shape.positions.size ()) throw std::invalid_argument ("Invalid model index");
                replacement.push_back (uint32_t (number));
            }
            shape.indices = std::move (replacement);
        } catch (...) { JS_FreeValue (ctx, indices); throw; }
    }
    JS_FreeValue (ctx, indices);
    for (uint32_t index : shape.indices)
        if (index >= shape.positions.size ()) throw std::invalid_argument ("Updated model index out of range");
}

JSValue model_apply_data (JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv) {
    auto* handle = model_handle (ctx, thisValue);
    if (!handle) return JS_ThrowTypeError (ctx, "applyData requires IModelData");
    if ((*handle)->destroyed) return JS_ThrowTypeError (ctx, "Model data has been destroyed");
    if (argc != 1 || !JS_IsObject (argv[0]))
        return JS_ThrowTypeError (ctx, "applyData requires model update");
    auto replacements = (*handle)->shapes;
    JSValue shapes = JS_UNDEFINED;
    try {
        shapes = model_property (ctx, argv[0], "shapes");
        if (JS_IsUndefined (shapes)) {
            if (replacements.size () != 1) throw std::invalid_argument ("Multi-shape applyData requires shapes array");
            model_apply_shape (ctx, replacements.front (), argv[0]);
        } else {
            JSValue lengthValue = model_property (ctx, shapes, "length");
            uint32_t length = 0;
            const int status = JS_ToUint32 (ctx, &length, lengthValue);
            JS_FreeValue (ctx, lengthValue);
            if (status < 0 || length != replacements.size ()) throw std::invalid_argument ("Model shape update count mismatch");
            for (uint32_t i = 0; i < length; ++i) {
                JSValue shape = JS_GetPropertyUint32 (ctx, shapes, i);
                try { model_apply_shape (ctx, replacements[i], shape); }
                catch (...) { JS_FreeValue (ctx, shape); throw; }
                JS_FreeValue (ctx, shape);
            }
        }
        JS_FreeValue (ctx, shapes);
        (*handle)->shapes = std::move (replacements);
        ++(*handle)->revision;
        return JS_UNDEFINED;
    } catch (const std::exception& error) {
        JS_FreeValue (ctx, shapes);
        return JS_ThrowTypeError (ctx, "%s", error.what ());
    }
}

JSValue scene_create_model_data (JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv) {
    auto* owner = get_opaque (ctx, thisValue);
    if (!owner) return JS_EXCEPTION;
    if (owner->getEngine ().isEvaluatingModuleTopLevel ())
        return JS_ThrowTypeError (ctx, "createModelData cannot be called from global scope");
    if (argc != 1 || !JS_IsObject (argv[0])) return JS_ThrowTypeError (ctx, "createModelData requires configuration");
    try {
        auto handle = std::make_unique<ModelHandle> (std::make_shared<DynamicModelData> ());
        (*handle)->shapes = model_shapes (ctx, owner->getEngine (), argv[0]);
        JSValue value = JS_NewObjectClass (ctx, owner->getModelDataClassId ());
        if (JS_IsException (value)) return value;
        JS_SetOpaque (value, handle.release ());
        JS_SetPropertyStr (ctx, value, "applyData", JS_NewCFunction (ctx, model_apply_data, "applyData", 1));
        JS_SetPropertyStr (ctx, value, "replaceData", JS_NewCFunction (ctx, model_replace_data, "replaceData", 1));
        return value;
    } catch (const std::exception& error) { return JS_ThrowTypeError (ctx, "%s", error.what ()); }
}

JSValue scene_destroy_model_data (JSContext* ctx, JSValueConst thisValue, int argc, JSValueConst* argv) {
    auto* owner = get_opaque (ctx, thisValue);
    if (!owner) return JS_EXCEPTION;
    if (owner->getEngine ().isEvaluatingModuleTopLevel ())
        return JS_ThrowTypeError (ctx, "destroyModelData cannot be called from global scope");
    auto* handle = argc == 1 ? model_handle (ctx, argv[0]) : nullptr;
    if (!handle) return JS_ThrowTypeError (ctx, "destroyModelData requires IModelData");
    (*handle)->destroyed = true;
    return JS_UNDEFINED;
}
}

SceneObject* get_opaque (JSContext* ctx, JSValueConst this_val) {
    const auto* engine = static_cast<ScriptEngine*> (JS_GetContextOpaque (ctx));
    if (!engine || !engine->getSceneObject ()) return nullptr;
    return static_cast<SceneObject*> (JS_GetOpaque2 (ctx, this_val, engine->getSceneObject ()->getClassId ()));
}

JSValue get_bloom (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (ctx, this_val);
    if (!container) return JS_EXCEPTION;

    return JS_NewBool (ctx, container->getScene ().getScene ().camera.bloom.enabled->value->getBool ());
}

JSValue get_bloomstrength (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (ctx, this_val);
    if (!container) return JS_EXCEPTION;

    return JS_NewInt32 (ctx, container->getScene ().getScene ().camera.bloom.strength->value->getInt ());
}

JSValue get_bloomthreshold (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (ctx, this_val);
    if (!container) return JS_EXCEPTION;

    return JS_NewInt32 (ctx, container->getScene ().getScene ().camera.bloom.threshold->value->getInt ());
}

JSValue get_clearenabled (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (ctx, this_val);
    if (!container) return JS_EXCEPTION;

    return JS_NewBool (ctx, container->getScene ().getScene ().camera.bloom.enabled->value->getBool ());
}

JSValue get_clearcolor (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (ctx, this_val);
    if (!container) return JS_EXCEPTION;

    return container->getEngine ().getAdapters ().vec3->instantiate (
	*container->getScene ().getScene ().colors.clear->value
    );
}

JSValue get_ambientcolor (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (ctx, this_val);
    if (!container) return JS_EXCEPTION;

    return container->getEngine ().getAdapters ().vec3->instantiate (
	*container->getScene ().getScene ().colors.ambient->value
    );
}

JSValue get_skylightcolor (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (ctx, this_val);
    if (!container) return JS_EXCEPTION;

    return container->getEngine ().getAdapters ().vec3->instantiate (
	*container->getScene ().getScene ().colors.ambient->value
    );
}

JSValue get_fov (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (ctx, this_val);
    if (!container) return JS_EXCEPTION;

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.projection.fov->value->getFloat ());
}

JSValue get_nearz (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (ctx, this_val);
    if (!container) return JS_EXCEPTION;

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.projection.nearz->value->getFloat ());
}

JSValue get_farz (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (ctx, this_val);
    if (!container) return JS_EXCEPTION;

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.projection.farz->value->getFloat ());
}

JSValue get_camerafade (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (ctx, this_val);
    if (!container) return JS_EXCEPTION;

    return JS_NewBool (ctx, container->getScene ().getScene ().camera.fade->value->getBool ());
}

JSValue get_camerashake (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (ctx, this_val);
    if (!container) return JS_EXCEPTION;

    return JS_NewBool (ctx, container->getScene ().getScene ().camera.shake.enabled->value->getBool ());
}

JSValue get_camerashakespeed (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (ctx, this_val);
    if (!container) return JS_EXCEPTION;

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.shake.speed->value->getFloat ());
}

JSValue get_camerashakeamplitude (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (ctx, this_val);
    if (!container) return JS_EXCEPTION;

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.shake.amplitude->value->getFloat ());
}

JSValue get_camerashakeroughness (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (ctx, this_val);
    if (!container) return JS_EXCEPTION;

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.shake.roughness->value->getFloat ());
}

JSValue get_cameraparallax (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (ctx, this_val);
    if (!container) return JS_EXCEPTION;

    return JS_NewBool (ctx, container->getScene ().getScene ().camera.parallax.enabled->value->getBool ());
}

JSValue get_cameraparallaxamount (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (ctx, this_val);
    if (!container) return JS_EXCEPTION;

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.parallax.amount->value->getFloat ());
}

JSValue get_cameraparallaxdelay (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (ctx, this_val);
    if (!container) return JS_EXCEPTION;

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.parallax.delay->value->getFloat ());
}

JSValue get_cameraparallaxmouseinfluence (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (ctx, this_val);
    if (!container) return JS_EXCEPTION;

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.parallax.mouseInfluence->value->getFloat ());
}

JSValue get_layer (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc != 1) {
	return JS_ThrowTypeError (ctx, "getLayer expects one layer name or index");
    }

    auto* container = get_opaque (ctx, this_val);
    if (!container) return JS_EXCEPTION;

    JSValue layer = argv[0];

    if (JS_IsNumber (layer)) {
	int index = 0;

	if (JS_ToInt32 (ctx, &index, layer) < 0) return JS_EXCEPTION;

	const auto& order = container->getScene ().getScriptLayers ();
	if (index < 0 || static_cast<size_t> (index) >= order.size ()) return JS_UNDEFINED;
	auto* object = order[index];

	if (object == nullptr) {
	    return JS_UNDEFINED;
	}

	if (!object->is<ScriptableObject> ()) {
	    return JS_UNDEFINED;
	}

	// TODO: REMOVE THIS CONST_CAST?
	return container->getEngine ().getAdapters ().object->instantiate (
	    const_cast<ScriptableObject&> (*object->as<ScriptableObject> ())
	);
    } else if (JS_IsString (layer)) {
	// find by name, this is harder
	const char* result = JS_ToCString (ctx, layer);

	if (result == nullptr) {
	    return JS_EXCEPTION;
	}

	ScopeGuard guard ([=] { JS_FreeCString (ctx, result); });

	for (auto object : container->getScene ().getScriptLayers ()) {
	    if (object->getObject ().name != result) {
		continue;
	    }

	    if (!object->is<ScriptableObject> ()) {
		continue;
	    }

	    return container->getEngine ().getAdapters ().object->instantiate (*object->as<ScriptableObject> ());
	}
	// The native getLayer(name) API returns null when no named layer exists.
	return JS_NULL;
    }

    // Other unsupported argument types have no native lookup evidence.
    return JS_UNDEFINED;
}

static const WallpaperEngine::Render::CObject* resolve_scene_layer (
    JSContext* ctx, SceneObject& owner, JSValueConst value) {
    if (auto* layer = WallpaperEngine::Scripting::Adapters::ScriptableObjectAdapter::resolve (value))
        return layer;
    if (JS_IsNumber (value)) {
        int index = 0;
        if (JS_ToInt32 (ctx, &index, value) < 0) return nullptr;
        const auto& order = owner.getScene ().getScriptLayers ();
        return index >= 0 && static_cast<size_t> (index) < order.size () ? order[index] : nullptr;
    }
    if (JS_IsString (value)) {
        const char* name = JS_ToCString (ctx, value);
        if (!name) return nullptr;
        const std::string requested (name);
        JS_FreeCString (ctx, name);
        for (const auto* object : owner.getScene ().getScriptLayers ())
            if (object->getObject ().name == requested) return object;
    }
    return nullptr;
}

JSValue scene_set_camera_transforms (JSContext* ctx, JSValueConst thisValue,
                                     int argc, JSValueConst* argv) {
    auto* owner = get_opaque (ctx, thisValue);
    if (!owner) return JS_EXCEPTION;
    if (owner->getEngine ().isEvaluatingModuleTopLevel ())
        return JS_ThrowTypeError (ctx, "setCameraTransforms cannot be used in global scope");
    if (argc == 0 || !JS_IsObject (argv[0])) return JS_FALSE;
    const auto values = readSceneCameraTransforms (ctx, argv[0]);
    if (!values) return JS_EXCEPTION;
    if (!owner->getEngine ().isInitializingAuthoredLayer ())
        owner->getMutableScene ().getCamera ().setTransforms (
            values->eye ? &*values->eye : nullptr, values->center ? &*values->center : nullptr,
            values->up ? &*values->up : nullptr, values->zoom ? &*values->zoom : nullptr);
    return JS_TRUE;
}

JSValue scene_enumerate_layers (JSContext* ctx, JSValueConst thisValue, int, JSValueConst*) {
    auto* owner = get_opaque (ctx, thisValue);
    if (!owner) return JS_EXCEPTION;
    if (owner->getEngine ().isEvaluatingModuleTopLevel ())
        return JS_ThrowTypeError (ctx, "enumerateLayers cannot be used in global scope");
    JSValue layers = JS_NewArray (ctx);
    if (JS_IsException (layers)) return layers;
    uint32_t index = 0;
    for (const auto* object : owner->getScene ().getScriptLayers ()) {
        if (!object || !object->is<ScriptableObject> ()) continue;
        JSValue layer = owner->getEngine ().getAdapters ().object->instantiate (
            const_cast<ScriptableObject&> (*object->as<ScriptableObject> ()));
        if (JS_IsException (layer) || JS_SetPropertyUint32 (ctx, layers, index++, layer) < 0) {
            JS_FreeValue (ctx, layers);
            return JS_EXCEPTION;
        }
    }
    return layers;
}

JSValue scene_get_initial_layer_config (JSContext* ctx, JSValueConst thisValue,
                                       int argc, JSValueConst* argv) {
    auto* owner = get_opaque (ctx, thisValue);
    if (!owner) return JS_EXCEPTION;
    if (owner->getEngine ().isEvaluatingModuleTopLevel ())
        return JS_ThrowTypeError (ctx, "getInitialLayerConfig cannot be used in global scope");
    const auto* layer = argc > 0 ? resolve_scene_layer (ctx, *owner, argv[0]) : nullptr;
    if (!layer || layer->getObject ().initialConfiguration.empty ()) return JS_NULL;
    const auto& json = layer->getObject ().initialConfiguration;
    // Parsing a new object on every call detaches nested effects, settings and
    // arrays from both the authored snapshot and other callers' copies.
    return JS_ParseJSON (ctx, json.data (), json.size (), "initial layer configuration");
}

static std::string vector_config_string (JSContext* ctx, JSValueConst value, int dimensions) {
    std::ostringstream encoded;
    encoded.imbue (std::locale::classic ());
    encoded << std::setprecision (std::numeric_limits<double>::max_digits10);
    const bool array = JS_IsArray (value) == 1;
    const char* keys[] = {"x", "y", "z", "w"};
    for (int component = 0; component < dimensions; ++component) {
        JSValue part = array ? JS_GetPropertyUint32 (ctx, value, component)
                             : JS_GetPropertyStr (ctx, value, keys[component]);
        if (JS_IsException (part)) throw std::invalid_argument ("Cannot read layer vector component");
        double number = 0.0;
        const bool valid = JS_IsNumber (part) && JS_ToFloat64 (ctx, &number, part) == 0
            && std::isfinite (number);
        JS_FreeValue (ctx, part);
        if (!valid) throw std::invalid_argument ("Layer vector requires finite numeric components");
        if (component) encoded << ' ';
        encoded << number;
    }
    return encoded.str ();
}

static JSValue shallow_layer_config (JSContext* ctx, ScriptEngine& engine, JSValueConst source,
                                     int valueDimensions = 0) {
    JSPropertyEnum* names = nullptr;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames (ctx, &names, &count, source,
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return JS_EXCEPTION;
    ScopeGuard namesGuard ([&] {
        for (uint32_t index = 0; index < count; ++index) JS_FreeAtom (ctx, names[index].atom);
        js_free (ctx, names);
    });
    JSValue result = JS_NewObject (ctx);
    if (JS_IsException (result)) return JS_EXCEPTION;
    try {
        for (uint32_t index = 0; index < count; ++index) {
            const char* key = JS_AtomToCString (ctx, names[index].atom);
            if (!key) throw std::invalid_argument ("Invalid layer configuration key");
            const std::string fieldName (key);
            JS_FreeCString (ctx, key);
            JSValue field = JS_GetProperty (ctx, source, names[index].atom);
            if (JS_IsException (field)) throw std::invalid_argument ("Invalid layer configuration value");
            ScopeGuard fieldGuard ([&] { JS_FreeValue (ctx, field); });
            if (const auto asset = engine.registeredAssetPath (field)) {
                JS_FreeValue (ctx, field);
                field = JS_NewStringLen (ctx, asset->data (), asset->size ());
                if (JS_IsException (field)) throw std::invalid_argument ("Cannot encode registered asset");
            }
            if (fieldName == "model" && model_handle (ctx, field)) {
                JS_FreeValue (ctx, field);
                field = JS_NewString (ctx, "__dynamic_model__");
            }
            int dimensions = 0;
            if (valueDimensions && fieldName == "value") dimensions = valueDimensions;
            else if (fieldName == "origin" || fieldName == "angles" || fieldName == "scale"
                || fieldName == "color" || fieldName == "backgroundcolor") dimensions = 3;
            else if (fieldName == "size" || fieldName == "parallaxDepth") dimensions = 2;
            if (dimensions && JS_IsObject (field)) {
                int setting = 0;
                if (!valueDimensions) {
                    JSAtom valueAtom = JS_NewAtom (ctx, "value");
                    if (valueAtom == JS_ATOM_NULL) throw std::invalid_argument ("Cannot inspect layer vector");
                    setting = JS_GetOwnProperty (ctx, nullptr, field, valueAtom);
                    JS_FreeAtom (ctx, valueAtom);
                    if (setting < 0) throw std::invalid_argument ("Cannot inspect layer vector");
                }
                if (setting) {
                    JSValue wrapper = shallow_layer_config (ctx, engine, field, dimensions);
                    if (JS_IsException (wrapper)) throw std::invalid_argument ("Cannot copy layer vector setting");
                    JS_FreeValue (ctx, field);
                    field = wrapper;
                } else {
                    const std::string encoded = vector_config_string (ctx, field, dimensions);
                    JS_FreeValue (ctx, field);
                    field = JS_NewString (ctx, encoded.c_str ());
                    if (JS_IsException (field)) throw std::invalid_argument ("Cannot encode layer vector");
                }
            }
            const int written = JS_SetProperty (ctx, result, names[index].atom, JS_DupValue (ctx, field));
            if (written < 0) throw std::invalid_argument ("Cannot write layer configuration value");
        }
    } catch (...) {
        JS_FreeValue (ctx, result);
        throw;
    }
    return result;
}

JSValue scene_create_layer (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* owner = get_opaque (ctx, this_val);
    if (!owner) return JS_EXCEPTION;
    if (owner->getEngine ().isEvaluatingModuleTopLevel ())
        return JS_ThrowTypeError (ctx, "createLayer cannot be called from global scope");
    if (!owner || argc != 1 || (!JS_IsObject (argv[0]) && !JS_IsString (argv[0])))
        return JS_ThrowTypeError (ctx, "createLayer expects a layer configuration object or asset path");
    std::string configuration;
    ModelHandle dynamicModel;
    if (const auto asset = owner->getEngine ().registeredAssetPath (argv[0])) {
        const std::filesystem::path path (*asset);
        const auto generic = path.generic_string ();
        std::string field;
        if (generic.starts_with ("models/"))
            field = path.extension () == ".mdl" ? "model" : "image";
        else if (generic.starts_with ("particles/")) field = "particle";
        else if (generic.starts_with ("sounds/")) field = "sound";
        else return JS_ThrowTypeError (ctx, "createLayer cannot use this registered asset as a layer");
        configuration = WallpaperEngine::Data::JSON::JSON {{field, *asset}}.dump ();
    } else if (JS_IsString (argv[0])) {
        const char* path = JS_ToCString (ctx, argv[0]);
        if (!path) return JS_EXCEPTION;
        std::string resolvedPath = path;
        JS_FreeCString (ctx, path);
        if (const auto* module = owner->getEngine ().getRunningModule (); module && JS_IsObject (module->module)) {
            JSValue id = JS_GetPropertyStr (ctx, module->module, "__workshopId");
            if (JS_IsString (id)) {
                const char* workshopId = JS_ToCString (ctx, id);
                if (workshopId) {
                    const auto exists = [&] (const std::string& filename) {
                        try {
                            return bool (owner->getScene ().getScene ().project.assetLocator->read (filename));
                        } catch (const std::exception&) {
                            return false;
                        }
                    };
                    resolvedPath = resolveWorkshopScriptAssetPath (resolvedPath, workshopId, exists);
                    JS_FreeCString (ctx, workshopId);
                }
            }
            JS_FreeValue (ctx, id);
        }
        configuration = WallpaperEngine::Data::JSON::JSON {{"image", resolvedPath}}.dump ();
    } else {
        JSValue modelField = JS_GetPropertyStr (ctx, argv[0], "model");
        if (JS_IsException (modelField)) return JS_EXCEPTION;
        if (auto* handle = model_handle (ctx, modelField)) {
            if ((*handle)->destroyed) { JS_FreeValue (ctx, modelField); return JS_ThrowTypeError (ctx, "Model data has been destroyed"); }
            dynamicModel = *handle;
        }
        JS_FreeValue (ctx, modelField);
        JSValue normalized = JS_UNDEFINED;
        try {
            normalized = shallow_layer_config (ctx, owner->getEngine (), argv[0]);
        } catch (const std::exception& error) {
            return JS_ThrowTypeError (ctx, "createLayer configuration failed: %s", error.what ());
        }
        if (JS_IsException (normalized)) return JS_EXCEPTION;
        JSValue encoded = JS_JSONStringify (ctx, normalized, JS_UNDEFINED, JS_UNDEFINED);
        JS_FreeValue (ctx, normalized);
        if (JS_IsException (encoded)) return JS_EXCEPTION;
        const char* json = JS_ToCString (ctx, encoded);
        if (!json) {
            JS_FreeValue (ctx, encoded);
            return JS_EXCEPTION;
        }
        configuration = json;
        JS_FreeCString (ctx, json);
        JS_FreeValue (ctx, encoded);
    }
    try {
        auto* object = owner->getMutableScene ().createScriptLayer (configuration, dynamicModel);
        auto* scriptable = dynamic_cast<ScriptableObject*> (object);
        if (!scriptable) return JS_ThrowTypeError (ctx, "Created layer has no script interface");
        return owner->getEngine ().getAdapters ().object->instantiate (*scriptable);
    } catch (const std::exception& error) {
        sLog.error ("SceneScript createLayer: ", error.what ());
        return JS_ThrowTypeError (ctx, "createLayer failed: %s", error.what ());
    }
}

JSValue scene_get_layer_index (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* owner = get_opaque (ctx, this_val);
    if (!owner) return JS_EXCEPTION;
    if (!owner || argc != 1) return JS_ThrowTypeError (ctx, "getLayerIndex expects a layer");
    const auto* layer = resolve_scene_layer (ctx, *owner, argv[0]);
    const auto layers = owner->getScene ().getScriptLayers ();
    const auto found = std::find (layers.begin (), layers.end (), layer);
    return JS_NewInt32 (ctx, found == layers.end () ? -1 : static_cast<int> (found - layers.begin ()));
}

JSValue scene_sort_layer (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* owner = get_opaque (ctx, this_val);
    if (!owner) return JS_EXCEPTION;
    if (!owner || argc != 2) return JS_ThrowTypeError (ctx, "sortLayer expects a layer and index");
    int index = -1;
    if (JS_ToInt32 (ctx, &index, argv[1]) < 0) return JS_EXCEPTION;
    const auto* layer = resolve_scene_layer (ctx, *owner, argv[0]);
    const auto layers = owner->getScene ().getScriptLayers ();
    if (!layer || std::find (layers.begin (), layers.end (), layer) == layers.end ()
        || index < 0 || static_cast<size_t> (index) >= layers.size ()) return JS_FALSE;
    const int physicalIndex = owner->getScene ().getScriptLayerIndex (layers[index]);
    return JS_NewBool (ctx, owner->getMutableScene ().sortScriptLayer (layer, physicalIndex));
}

JSValue scene_destroy_layer (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* owner = get_opaque (ctx, this_val);
    if (!owner) return JS_EXCEPTION;
    if (!owner || argc != 1) return JS_ThrowTypeError (ctx, "destroyLayer expects a layer");
    const auto* layer = resolve_scene_layer (ctx, *owner, argv[0]);
    return JS_NewBool (ctx, owner->getMutableScene ().destroyScriptLayer (layer));
}

JSValue scene_set_value (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) { return JS_EXCEPTION; }

SceneObject::SceneObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene) :
    m_scene (scene), m_engine (engine), m_classId (0), m_modelDataClassId (0) {
    this->m_definition = { .class_name = "IScene" };
    this->m_classId = registerSceneScriptClass (this->m_engine.getRuntime (), this->m_definition);
    JSClassDef modelDefinition {.class_name = "IModelData", .finalizer = model_data_finalizer};
    m_modelDataClassId = registerSceneScriptClass (this->m_engine.getRuntime (), modelDefinition);
    JSValue modelConstants = JS_NewObject (this->m_engine.getContext ());
    for (const auto& [name, value] : {std::pair {"POSITION", 0}, {"NORMAL", 1},
                                     {"UV", 2}, {"TANGENT_SIGNED", 3}})
        JS_SetPropertyStr (this->m_engine.getContext (), modelConstants, name,
                           JS_NewInt32 (this->m_engine.getContext (), value));
    JSValue global = JS_GetGlobalObject (this->m_engine.getContext ());
    JS_SetPropertyStr (this->m_engine.getContext (), global, "IModelData", modelConstants);
    JS_FreeValue (this->m_engine.getContext (), global);
    this->m_instance = JS_NewObjectClass (this->m_engine.getContext (), this->m_classId);

    JS_DupValue (this->m_engine.getContext (), this->m_instance);

    // set properties
    JS_SetOpaque (this->m_instance, this);
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "bloom"),
	JS_NewCFunction (this->m_engine.getContext (), get_bloom, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), scene_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "bloomstrength"),
	JS_NewCFunction (this->m_engine.getContext (), get_bloomstrength, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), scene_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "bloomthreshold"),
	JS_NewCFunction (this->m_engine.getContext (), get_bloomthreshold, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), scene_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "clearenabled"),
	JS_NewCFunction (this->m_engine.getContext (), get_clearenabled, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), scene_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "clearcolor"),
	JS_NewCFunction (this->m_engine.getContext (), get_clearcolor, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), scene_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "ambientcolor"),
	JS_NewCFunction (this->m_engine.getContext (), get_ambientcolor, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), scene_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "skylightcolor"),
	JS_NewCFunction (this->m_engine.getContext (), get_skylightcolor, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), scene_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "fov"),
	JS_NewCFunction (this->m_engine.getContext (), get_fov, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), scene_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "nearz"),
	JS_NewCFunction (this->m_engine.getContext (), get_nearz, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), scene_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "farz"),
	JS_NewCFunction (this->m_engine.getContext (), get_farz, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), scene_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "camerafade"),
	JS_NewCFunction (this->m_engine.getContext (), get_camerafade, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), scene_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "camerashake"),
	JS_NewCFunction (this->m_engine.getContext (), get_camerashake, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), scene_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "camerashakespeed"),
	JS_NewCFunction (this->m_engine.getContext (), get_camerashakespeed, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), scene_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance,
	JS_NewAtom (this->m_engine.getContext (), "camerashakeamplitude"),
	JS_NewCFunction (this->m_engine.getContext (), get_camerashakeamplitude, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), scene_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance,
	JS_NewAtom (this->m_engine.getContext (), "camerashakeroughness"),
	JS_NewCFunction (this->m_engine.getContext (), get_camerashakeroughness, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), scene_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "cameraparallax"),
	JS_NewCFunction (this->m_engine.getContext (), get_cameraparallax, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), scene_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance,
	JS_NewAtom (this->m_engine.getContext (), "cameraparallaxamount"),
	JS_NewCFunction (this->m_engine.getContext (), get_cameraparallaxamount, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), scene_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance,
	JS_NewAtom (this->m_engine.getContext (), "cameraparallaxdelay"),
	JS_NewCFunction (this->m_engine.getContext (), get_cameraparallaxdelay, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), scene_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance,
	JS_NewAtom (this->m_engine.getContext (), "cameraparallaxmouseinfluence"),
	JS_NewCFunction (this->m_engine.getContext (), get_cameraparallaxmouseinfluence, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), scene_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "getLayer",
	JS_NewCFunction (this->m_engine.getContext (), get_layer, "getLayer", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
        this->m_engine.getContext (), this->m_instance, "setCameraTransforms",
        JS_NewCFunction (this->m_engine.getContext (), scene_set_camera_transforms, "setCameraTransforms", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
        this->m_engine.getContext (), this->m_instance, "enumerateLayers",
        JS_NewCFunction (this->m_engine.getContext (), scene_enumerate_layers, "enumerateLayers", 0), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
        this->m_engine.getContext (), this->m_instance, "getInitialLayerConfig",
        JS_NewCFunction (this->m_engine.getContext (), scene_get_initial_layer_config, "getInitialLayerConfig", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "createLayer",
	JS_NewCFunction (this->m_engine.getContext (), scene_create_layer, "createLayer", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
        this->m_engine.getContext (), this->m_instance, "createModelData",
        JS_NewCFunction (this->m_engine.getContext (), scene_create_model_data, "createModelData", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
        this->m_engine.getContext (), this->m_instance, "destroyModelData",
        JS_NewCFunction (this->m_engine.getContext (), scene_destroy_model_data, "destroyModelData", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "getLayerIndex",
	JS_NewCFunction (this->m_engine.getContext (), scene_get_layer_index, "getLayerIndex", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "sortLayer",
	JS_NewCFunction (this->m_engine.getContext (), scene_sort_layer, "sortLayer", 2), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
        this->m_engine.getContext (), this->m_instance, "destroyLayer",
        JS_NewCFunction (this->m_engine.getContext (), scene_destroy_layer, "destroyLayer", 1), JS_PROP_ENUMERABLE
    );
    // TODO: ADD REST OF THE METHODS
}

SceneObject::~SceneObject () { JS_FreeValue (this->m_engine.getContext (), this->m_instance); }
