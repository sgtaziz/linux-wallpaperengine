#include "VectorAdapter.h"

#include "../ScriptEngine.h"
#include "WallpaperEngine/Data/Utils/SFINAE.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"

#include <cmath>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <variant>

using namespace WallpaperEngine::Data::Utils;
using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Scripting::Adapters;

static uint32_t VectorInstanceId = 0;
static uint32_t VectorAdapterInstanceId = 0;
static constexpr int InvalidVectorInstanceId = 0;

// magic value used to ensure the assigned opaque value we got back is valid
#define VEC_OPAQUE_MAGIC 0xdeadbee0
#define VEC_MAGIC_CHECK_EXCEPTION(container, components)                                                               \
    do {                                                                                                               \
	if (!container || container->magic != (int)(VEC_OPAQUE_MAGIC + components)) {                                  \
	    return JS_ThrowTypeError (ctx, "Invalid Vec%d receiver", components);                                    \
	}                                                                                                              \
    } while (0)
#define VEC_MAGIC_CHECK_ERROR(container, components)                                                                   \
    do {                                                                                                               \
	if (!container || container->magic != (int)(VEC_OPAQUE_MAGIC + components)) {                                  \
	    JS_ThrowTypeError (ctx, "Invalid Vec%d receiver", components);                                           \
	    return -1;                                                                                                 \
	}                                                                                                              \
    } while (0)

template <int components> std::map<uint32_t, VectorAdapter<components>&> vectorAdapterInstances;

template <int components> struct VectorOpaqueContainer {
    int magic;
    VectorAdapter<components>& adapter;
    DynamicValue& value;
    uint32_t id;
    DynamicValue* layerProperty = nullptr;
    std::function<bool ()> layerAlive;
};

template <int components> auto vector_new () -> decltype (auto) {
    static_assert (components >= 2 && components <= 4, "Unsupported vector type");

    if constexpr (components == 2) {
	return glm::vec2 {};
    } else if constexpr (components == 3) {
	return glm::vec3 {};
    } else if constexpr (components == 4) {
	return glm::vec4 {};
    }
}

template auto vector_new<2> () -> decltype (auto);
template auto vector_new<3> () -> decltype (auto);
template auto vector_new<4> () -> decltype (auto);

template <int components> auto vector_new (float value) -> decltype (auto) {
    static_assert (components >= 2 && components <= 4, "Unsupported vector type");

    if constexpr (components == 2) {
	return glm::vec2 (value);
    } else if constexpr (components == 3) {
	return glm::vec3 (value);
    } else if constexpr (components == 4) {
	return glm::vec4 (value);
    }
}

template auto vector_new<2> (float value) -> decltype (auto);
template auto vector_new<3> (float value) -> decltype (auto);
template auto vector_new<4> (float value) -> decltype (auto);

template <int components> auto vector_get (DynamicValue& value) -> decltype (auto) {
    static_assert (components >= 2 && components <= 4, "Unsupported vector type");

    if constexpr (components == 2) {
	return value.getVec2 ();
    } else if constexpr (components == 3) {
	return value.getVec3 ();
    } else if constexpr (components == 4) {
	return value.getVec4 ();
    }
}

template <int components> auto vector_get (JSContext* ctx, JSValue source) -> decltype (auto) {
    static_assert (components >= 2 && components <= 4, "Unsupported vector type");

    int tag = JS_VALUE_GET_TAG (source);

    if (tag == JS_TAG_INT) {
	int32_t value = 0;

	JS_ToInt32 (ctx, &value, source);

	return vector_new<components> (value);
    }

    if (JS_TAG_IS_FLOAT64 (tag)) {
	double value = 0.0f;

	JS_ToFloat64 (ctx, &value, source);

	return vector_new<components> (static_cast<float> (value));
    }

    if (tag == JS_TAG_OBJECT) {
	// Only read the requested components. Vector exotic objects report an
	// exception for unknown properties, so probing w on a Vec3 leaves a
	// pending JS exception even when the copy itself succeeds.
	const char* keys[] = {"x", "y", "z", "w"};
	JSValue parts[4] = {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};
	ScopeGuard partGuard ([&] {
	    for (auto& part : parts) JS_FreeValue (ctx, part);
	});
	float values[4] {};
	for (int index = 0; index < components; ++index) {
	    parts[index] = JS_GetPropertyStr (ctx, source, keys[index]);
	    double numeric = 0.0;
	    if (!JS_IsNumber (parts[index]) || JS_ToFloat64 (ctx, &numeric, parts[index]) < 0
	        || !std::isfinite (numeric) || std::abs (numeric) > std::numeric_limits<float>::max ())
	        throw std::runtime_error ("Unsupported type conversion for VectorAdapter");
	    values[index] = static_cast<float> (numeric);
	}

	if constexpr (components == 2) {
	    return glm::vec2 (values[0], values[1]);
	} else if constexpr (components == 3) {
	    return glm::vec3 (values[0], values[1], values[2]);
	} else if constexpr (components == 4) {
	    return glm::vec4 (values[0], values[1], values[2], values[3]);
	}
    }

    throw std::runtime_error ("Unsupported type conversion for VectorAdapter");
}

template auto vector_get<2> (JSContext* ctx, JSValue source) -> decltype (auto);
template auto vector_get<3> (JSContext* ctx, JSValue source) -> decltype (auto);
template auto vector_get<4> (JSContext* ctx, JSValue source) -> decltype (auto);
template auto vector_get<2> (DynamicValue& value) -> decltype (auto);
template auto vector_get<3> (DynamicValue& value) -> decltype (auto);
template auto vector_get<4> (DynamicValue& value) -> decltype (auto);

// A bad SceneScript vector argument must raise a QuickJS exception. C++
// exceptions cannot unwind through QuickJS's C frames; doing so used to end
// the renderer on malformed calls such as Vec3(undefined).
template <int components>
auto vector_get_script (JSContext* ctx, JSValue source)
    -> std::optional<decltype (vector_new<components> ())> {
    try {
        return vector_get<components> (ctx, source);
    } catch (const std::runtime_error& error) {
        // A failed object property getter already owns a JS exception. A
        // primitive input never calls a getter, so ensure it has our TypeError
        // even if another QuickJS helper left a pending sentinel.
        if (!JS_IsObject (source) || !JS_HasException (ctx))
            JS_ThrowTypeError (ctx, "%s", error.what ());
        return std::nullopt;
    }
}

template <int components>
JSValue vector_property_get (JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst receiver) {
    JSClassID classId = 0;

    auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (obj_val, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (container, components);

    const char* name = JS_AtomToCString (ctx, atom);

    if (name == nullptr) {
	return JS_EXCEPTION;
    }

    ScopeGuard guard ([=] { JS_FreeCString (ctx, name); });
    const auto value = vector_get<components> (container->value);

    if (strcmp (name, "x") == 0) {
	return JS_NewFloat64 (ctx, value.x);
    }
    if (strcmp (name, "y") == 0) {
	return JS_NewFloat64 (ctx, value.y);
    }
    if constexpr (components >= 3) {
	if (strcmp (name, "z") == 0) {
	    return JS_NewFloat64 (ctx, value.z);
	}

	if constexpr (components >= 4) {
	    if (strcmp (name, "w") == 0) {
		return JS_NewFloat64 (ctx, value.w);
	    }
	}
    }

    // QuickJS does not continue prototype lookup after an exotic getter.
    // Retrieve methods from our plain prototype explicitly; unknown names
    // resolve to undefined like ordinary JavaScript properties.
    JSValue prototype = JS_GetPrototype (ctx, obj_val);
    if (JS_IsException (prototype)) return JS_EXCEPTION;
    JSValue member = JS_GetProperty (ctx, prototype, atom);
    JS_FreeValue (ctx, prototype);
    return member;
}

template JSValue vector_property_get<2> (JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst receiver);
template JSValue vector_property_get<3> (JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst receiver);
template JSValue vector_property_get<4> (JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst receiver);

// Shipped SceneScript Vec constructors assign x/y/z/w directly on the JS
// instance. QuickJS exotic getters alone make these values readable but fail
// hasOwnProperty/Object.keys, which authored scripts use to distinguish a
// vector from a scalar before applying component-wise animation.
template <int components>
int vector_own_property (JSContext* ctx, JSPropertyDescriptor* desc,
                         JSValueConst object, JSAtom atom) {
    JSClassID classId = 0;
    const auto* container = static_cast<VectorOpaqueContainer<components>*> (
        JS_GetAnyOpaque (object, &classId));
    VEC_MAGIC_CHECK_ERROR (container, components);
    const char* name = JS_AtomToCString (ctx, atom);
    if (!name) return -1;
    int index = -1;
    for (int i = 0; i < components; ++i)
        if (name[0] == "xyzw"[i] && name[1] == '\0') index = i;
    JS_FreeCString (ctx, name);
    if (index < 0) return 0;
    if (desc) {
        const auto value = vector_get<components> (container->value);
        desc->flags = JS_PROP_C_W_E | JS_PROP_HAS_VALUE;
        desc->value = JS_NewFloat64 (ctx, value[index]);
        desc->getter = JS_UNDEFINED;
        desc->setter = JS_UNDEFINED;
    }
    return 1;
}

template <int components>
int vector_own_property_names (JSContext* ctx, JSPropertyEnum** table,
                               uint32_t* length, JSValueConst object) {
    JSClassID classId = 0;
    const auto* container = static_cast<VectorOpaqueContainer<components>*> (
        JS_GetAnyOpaque (object, &classId));
    VEC_MAGIC_CHECK_ERROR (container, components);
    auto* fields = static_cast<JSPropertyEnum*> (js_malloc (ctx, sizeof (JSPropertyEnum) * components));
    if (!fields) return -1;
    for (int i = 0; i < components; ++i) {
        const char name[] = {"xyzw"[i], '\0'};
        fields[i] = {true, JS_NewAtom (ctx, name)};
        if (fields[i].atom == JS_ATOM_NULL) {
            for (int j = 0; j < i; ++j) JS_FreeAtom (ctx, fields[j].atom);
            js_free (ctx, fields);
            return -1;
        }
    }
    *table = fields;
    *length = components;
    return 0;
}

template <int components>
int vector_property_set (
    JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst val, JSValueConst receiver, int flags
) {
    JSClassID classId = 0;
    auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (obj_val, &classId));

    VEC_MAGIC_CHECK_ERROR (container, components);

    int tag = JS_VALUE_GET_TAG (val);

    if (tag != JS_TAG_INT && !JS_TAG_IS_FLOAT64 (tag)) {
	return -1;
    }

    const char* name = JS_AtomToCString (ctx, atom);

    if (name == nullptr) {
	return -1;
    }

    ScopeGuard guard ([=] { JS_FreeCString (ctx, name); });
    auto vec = vector_get<components> (container->value);
    void* into = nullptr;
    int componentIndex = -1;

    if (strcmp (name, "x") == 0) {
	if constexpr (
	    std::is_same_v<decltype (vec), glm::vec2> || std::is_same_v<decltype (vec), glm::vec3>
	    || std::is_same_v<decltype (vec), glm::vec4>
	) {
	    into = &vec.x;
	    componentIndex = 0;
	} else if constexpr (std::is_same_v<decltype (vec), Color>) {
	    into = &vec.r;
	    componentIndex = 0;
	}
    } else if (strcmp (name, "y") == 0) {
	if constexpr (
	    std::is_same_v<decltype (vec), glm::vec2> || std::is_same_v<decltype (vec), glm::vec3>
	    || std::is_same_v<decltype (vec), glm::vec4>
	) {
	    into = &vec.y;
	    componentIndex = 1;
	} else if constexpr (std::is_same_v<decltype (vec), Color>) {
	    into = &vec.g;
	    componentIndex = 1;
	}
    } else if constexpr (components >= 3) {
	if (strcmp (name, "z") == 0) {
	    if constexpr (std::is_same_v<decltype (vec), glm::vec3> || std::is_same_v<decltype (vec), glm::vec4>) {
		into = &vec.z;
		componentIndex = 2;
	    } else if constexpr (std::is_same_v<decltype (vec), Color>) {
		into = &vec.b;
		componentIndex = 2;
	    }
	} else if constexpr (components >= 4) {
	    if (strcmp (name, "w") == 0) {
		if constexpr (std::is_same_v<decltype (vec), glm::vec4>) {
		    into = &vec.w;
		    componentIndex = 3;
		} else if constexpr (std::is_same_v<decltype (vec), Color>) {
		    into = &vec.a;
		    componentIndex = 3;
		}
	    }
	}
    }

    if (into == nullptr) {
	return -1;
    }

    double value = 0;

    JS_ToFloat64 (ctx, &value, val);

    *static_cast<float*> (into) = static_cast<float> (value);

    if (container->layerProperty && (!container->layerAlive || !container->layerAlive ())) {
        JS_ThrowTypeError (ctx, "Layer vector is no longer available");
        return -1;
    }
    container->value.update (vec, DynamicValue::UpdateSource::Script);
    if (container->layerProperty) {
        // A retained property snapshot may predate a whole-vector assignment.
        // Apply only the edited component to today's layer vector, preserving
        // all other components written since the snapshot was captured.
        auto current = vector_get<components> (*container->layerProperty);
        current[componentIndex] = static_cast<float> (value);
        container->layerProperty->update (current, DynamicValue::UpdateSource::Script);
    }

    return 0;
}

template int vector_property_set<2> (
    JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst val, JSValueConst receiver, int flags
);
template int vector_property_set<3> (
    JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst val, JSValueConst receiver, int flags
);
template int vector_property_set<4> (
    JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst val, JSValueConst receiver, int flags
);

template <int components> JSValue vector_copy (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId = 0;

    auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (this_val, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (container, components);

    // create a new DynamicValue
    return container->adapter.instantiate (container->value, true);
}

template JSValue vector_copy<2> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_copy<3> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_copy<4> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

bool vector_value_equals (JSContext* ctx, JSValue left, JSValue right) {
    if (JS_TAG_IS_FLOAT64 (JS_VALUE_GET_TAG (left)) || JS_TAG_IS_FLOAT64 (JS_VALUE_GET_TAG (right))) {
	double x1Val = 0.0f, x2Val = 0.0f;

	JS_ToFloat64 (ctx, &x1Val, left);
	JS_ToFloat64 (ctx, &x2Val, right);

	if (std::abs (x1Val - x2Val) > 0.00001) {
	    return false;
	}
    } else {
	if (!JS_IsEqual (ctx, left, right)) {
	    return false;
	}
    }

    return true;
}

template <int components> JSValue vector_equals (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc == 0) {
	return JS_FALSE;
    }

    JSClassID classId = 0;

    auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (this_val, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (container, components);

    JSValue other = argv[0];
    auto* otherContainer = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (other, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (otherContainer, components);

    const auto vector = vector_get<components> (container->value);
    const auto otherVector = vector_get<components> (otherContainer->value);

    if constexpr (components == 2) {
	return vector.x == otherVector.x && vector.y == otherVector.y ? JS_TRUE : JS_FALSE;
    } else if constexpr (components == 3) {
	return vector.x == otherVector.x && vector.y == otherVector.y && vector.z == otherVector.z ? JS_TRUE : JS_FALSE;
    } else if constexpr (components == 4) {
	return vector.x == otherVector.x && vector.y == otherVector.y && vector.z == otherVector.z
		&& vector.w == otherVector.w
	    ? JS_TRUE
	    : JS_FALSE;
    }

    return JS_FALSE;
}

template JSValue vector_equals<2> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_equals<3> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_equals<4> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

template <int components> JSValue vector_length (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId = 0;

    auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (this_val, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (container, components);

    return JS_NewFloat64 (ctx, glm::length (vector_get<components> (container->value)));
}

template JSValue vector_length<2> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_length<3> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_length<4> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

template <int components>
JSValue vector_constructor (JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv, int magic) {
    auto it = vectorAdapterInstances<components>.find (magic);

    if (it == vectorAdapterInstances<components>.end ()) {
	return JS_EXCEPTION;
    }

    JSValue result = it->second.instantiate ();
    JSClassID classId = 0;
    auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (result, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (container, components);

    if (argc == 0) {
        // instantiate() already initializes every component to zero.
    } else if (argc == components || (components == 3 && argc == 2)) {
        float values[4] {};
        for (int index = 0; index < argc; ++index) {
            double numeric = 0.0;
            if (!JS_IsNumber (argv[index]) || JS_ToFloat64 (ctx, &numeric, argv[index]) < 0
                || !std::isfinite (numeric) || std::abs (numeric) > std::numeric_limits<float>::max ()) {
                JS_FreeValue (ctx, result);
                return JS_ThrowTypeError (ctx, "Vec%d components must be finite numbers", components);
            }
            values[index] = static_cast<float> (numeric);
        }
        if constexpr (components == 2)
            container->value.update (glm::vec2 (values[0], values[1]), DynamicValue::Initialization);
        else if constexpr (components == 3)
            container->value.update (glm::vec3 (values[0], values[1], values[2]), DynamicValue::Initialization);
        else
            container->value.update (glm::vec4 (values[0], values[1], values[2], values[3]), DynamicValue::Initialization);
    } else if (argc == 1 && JS_IsString (argv[0])) {
        const char* source = JS_ToCString (ctx, argv[0]);
        if (!source) { JS_FreeValue (ctx, result); return JS_EXCEPTION; }
        std::istringstream input (source);
        input.imbue (std::locale::classic ());
        JS_FreeCString (ctx, source);
        float values[4] {};
        for (int index = 0; index < components; ++index) {
            if (!(input >> values[index]) || !std::isfinite (values[index])) {
                JS_FreeValue (ctx, result);
                return JS_ThrowTypeError (ctx, "Vec%d string requires %d finite components", components, components);
            }
        }
        if constexpr (components == 2)
            container->value.update (glm::vec2 (values[0], values[1]), DynamicValue::Initialization);
        else if constexpr (components == 3)
            container->value.update (glm::vec3 (values[0], values[1], values[2]), DynamicValue::Initialization);
        else
            container->value.update (glm::vec4 (values[0], values[1], values[2], values[3]), DynamicValue::Initialization);
    } else if (argc == 1) {
        JSClassID classId = 0;
        auto* opaque = JS_IsObject (argv[0]) ? JS_GetAnyOpaque (argv[0], &classId) : nullptr;
        if constexpr (components == 3) {
            auto* smaller = static_cast<VectorOpaqueContainer<2>*> (opaque);
            if (smaller && smaller->magic == static_cast<int> (VEC_OPAQUE_MAGIC + 2)) {
                const auto value = vector_get<2> (smaller->value);
                container->value.update (glm::vec3 (value, 0.0f), DynamicValue::Initialization);
                return result;
            }
        }
        if constexpr (components == 4) {
            auto* two = static_cast<VectorOpaqueContainer<2>*> (opaque);
            if (two && two->magic == static_cast<int> (VEC_OPAQUE_MAGIC + 2)) {
                const auto value = vector_get<2> (two->value);
                container->value.update (glm::vec4 (value, 0.0f, 0.0f), DynamicValue::Initialization);
                return result;
            }
            auto* three = static_cast<VectorOpaqueContainer<3>*> (opaque);
            if (three && three->magic == static_cast<int> (VEC_OPAQUE_MAGIC + 3)) {
                const auto value = vector_get<3> (three->value);
                container->value.update (glm::vec4 (value, 0.0f), DynamicValue::Initialization);
                return result;
            }
        }
        const auto value = vector_get_script<components> (ctx, argv[0]);
        if (!value) { JS_FreeValue (ctx, result); return JS_EXCEPTION; }
        container->value.update (*value, DynamicValue::Initialization);
    } else {
        JS_FreeValue (ctx, result);
        return JS_ThrowTypeError (ctx, "Vec%d expects one value or %d components", components, components);
    }

    return result;
}

template JSValue vector_constructor<2> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);
template JSValue vector_constructor<3> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);
template JSValue vector_constructor<4> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic);

template <int components> void vector_finalizer (JSRuntime* rt, JSValueConst val) {
    JSClassID classId = 0;
    const auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (val, &classId));

    if (!container || container->magic != (int)(VEC_OPAQUE_MAGIC + components)) {
	return;
    }

    // free container and the associated DynamicValue if temporal
    if (container->id != InvalidVectorInstanceId) {
	container->adapter.free (container->id);
    }

    delete container;
}

template void vector_finalizer<2> (JSRuntime* rt, JSValueConst val);
template void vector_finalizer<3> (JSRuntime* rt, JSValueConst val);
template void vector_finalizer<4> (JSRuntime* rt, JSValueConst val);

template <int components>
JSValue vector_normalize (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId = 0;
    const auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (this_val, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (container, components);

    JSValue newVector = container->adapter.instantiate (container->value, true);

    const auto* newContainer = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (newVector, &classId));

    newContainer->value.update (
	glm::normalize (vector_get<components> (container->value)), DynamicValue::UpdateSource::Script
    );

    return newVector;
}

template JSValue vector_normalize<2> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_normalize<3> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_normalize<4> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

template <int components> JSValue vector_add (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc != 1) {
	return JS_UNDEFINED;
    }

    JSClassID classId = 0;
    const auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (this_val, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (container, components);

    const auto argument = vector_get_script<components> (ctx, argv[0]);
    if (!argument) return JS_EXCEPTION;

    JSValue newVector = container->adapter.instantiate ();
    const auto* newContainer = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (newVector, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (newContainer, components);

    newContainer->value.update (
	*argument + vector_get<components> (container->value),
	DynamicValue::UpdateSource::Initialization
    );

    return newVector;
}

template JSValue vector_add<2> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_add<3> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_add<4> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

template <int components>
JSValue vector_subtract (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc != 1) {
	return JS_UNDEFINED;
    }

    JSClassID classId = 0;
    const auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (this_val, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (container, components);

    const auto argument = vector_get_script<components> (ctx, argv[0]);
    if (!argument) return JS_EXCEPTION;

    JSValue newVector = container->adapter.instantiate ();
    const auto* newContainer = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (newVector, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (newContainer, components);

    newContainer->value.update (
	*argument - vector_get<components> (container->value),
	DynamicValue::UpdateSource::Initialization
    );

    return newVector;
}

template JSValue vector_subtract<2> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_subtract<3> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_subtract<4> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

template <int components>
JSValue vector_multiply (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc != 1) {
	return JS_UNDEFINED;
    }

    JSClassID classId = 0;
    const auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (this_val, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (container, components);

    const auto argument = vector_get_script<components> (ctx, argv[0]);
    if (!argument) return JS_EXCEPTION;

    JSValue newVector = container->adapter.instantiate ();
    const auto* newContainer = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (newVector, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (newContainer, components);

    newContainer->value.update (
	*argument * vector_get<components> (container->value),
	DynamicValue::UpdateSource::Initialization
    );

    return newVector;
}

template JSValue vector_multiply<2> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_multiply<3> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_multiply<4> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

template <int components> JSValue vector_divide (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc != 1) {
	return JS_UNDEFINED;
    }

    JSClassID classId = 0;
    const auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (this_val, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (container, components);

    const auto argument = vector_get_script<components> (ctx, argv[0]);
    if (!argument) return JS_EXCEPTION;

    JSValue newVector = container->adapter.instantiate ();
    const auto* newContainer = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (newVector, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (newContainer, components);

    newContainer->value.update (
	vector_get<components> (container->value) / *argument,
	DynamicValue::UpdateSource::Initialization
    );

    return newVector;
}

template JSValue vector_divide<2> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_divide<3> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_divide<4> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

template <int components> JSValue vector_dot (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc != 1) {
	return JS_UNDEFINED;
    }

    JSClassID classId = 0;
    const auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (this_val, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (container, components);

    const auto argument = vector_get_script<components> (ctx, argv[0]);
    if (!argument) return JS_EXCEPTION;

    JSValue newVector = container->adapter.instantiate ();
    const auto* newContainer = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (newVector, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (newContainer, components);

    newContainer->value.update (
	glm::dot (*argument, vector_get<components> (container->value)),
	DynamicValue::UpdateSource::Initialization
    );

    return newVector;
}

template JSValue vector_dot<2> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_dot<3> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_dot<4> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

template <int components> JSValue vector_cross (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc != 1) {
	return JS_UNDEFINED;
    }

    JSClassID classId = 0;
    const auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (this_val, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (container, components);

    const auto argument = vector_get_script<components> (ctx, argv[0]);
    if (!argument) return JS_EXCEPTION;

    JSValue newVector = container->adapter.instantiate ();
    const auto* newContainer = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (newVector, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (newContainer, components);

    newContainer->value.update (
	glm::cross (*argument, vector_get<components> (container->value)),
	DynamicValue::UpdateSource::Initialization
    );

    return newVector;
}

template JSValue vector_cross<3> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

template <int components> JSValue vector_mix (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc != 2) {
	return JS_EXCEPTION;
    }

    if (!JS_IsNumber (argv[1])) {
	return JS_EXCEPTION;
    }

    double amount = 0.0f;

    JS_ToFloat64 (ctx, &amount, argv[1]);

    JSClassID classId = 0;
    const auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (this_val, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (container, components);

    const auto argument = vector_get_script<components> (ctx, argv[0]);
    if (!argument) return JS_EXCEPTION;

    JSValue newVector = container->adapter.instantiate ();
    const auto* newContainer = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (newVector, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (newContainer, components);

    newContainer->value.update (
	glm::mix (vector_get<components> (container->value), *argument, amount),
	DynamicValue::UpdateSource::Initialization
    );

    return newVector;
}

template JSValue vector_mix<2> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_mix<3> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_mix<4> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

template <int components> JSValue vector_min (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc != 1) {
	return JS_UNDEFINED;
    }

    JSClassID classId = 0;
    const auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (this_val, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (container, components);

    const auto argument = vector_get_script<components> (ctx, argv[0]);
    if (!argument) return JS_EXCEPTION;

    JSValue newVector = container->adapter.instantiate ();
    const auto* newContainer = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (newVector, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (newContainer, components);

    newContainer->value.update (
	glm::min (*argument, vector_get<components> (container->value)),
	DynamicValue::UpdateSource::Initialization
    );

    return newVector;
}

template JSValue vector_min<2> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_min<3> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_min<4> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

template <int components> JSValue vector_max (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc != 1) {
	return JS_UNDEFINED;
    }

    JSClassID classId = 0;
    const auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (this_val, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (container, components);

    const auto argument = vector_get_script<components> (ctx, argv[0]);
    if (!argument) return JS_EXCEPTION;

    JSValue newVector = container->adapter.instantiate ();
    const auto* newContainer = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (newVector, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (newContainer, components);

    newContainer->value.update (
	glm::max (*argument, vector_get<components> (container->value)),
	DynamicValue::UpdateSource::Initialization
    );

    return newVector;
}

template JSValue vector_max<2> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_max<3> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_max<4> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

template <int components> JSValue vector_abs (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId = 0;
    const auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (this_val, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (container, components);

    JSValue newVector = container->adapter.instantiate ();
    const auto* newContainer = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (newVector, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (newContainer, components);

    newContainer->value.update (
	glm::abs (vector_get<components> (container->value)), DynamicValue::UpdateSource::Initialization
    );

    return newVector;
}

template JSValue vector_abs<2> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_abs<3> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_abs<4> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

template <int components> JSValue vector_sign (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId = 0;
    const auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (this_val, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (container, components);

    JSValue newVector = container->adapter.instantiate ();
    const auto* newContainer = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (newVector, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (newContainer, components);

    newContainer->value.update (
	glm::sign (vector_get<components> (container->value)), DynamicValue::UpdateSource::Initialization
    );

    return newVector;
}

template JSValue vector_sign<2> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_sign<3> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_sign<4> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

template <int components> JSValue vector_round (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId = 0;
    const auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (this_val, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (container, components);

    JSValue newVector = container->adapter.instantiate ();
    const auto* newContainer = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (newVector, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (newContainer, components);

    newContainer->value.update (
	glm::round (vector_get<components> (container->value)), DynamicValue::UpdateSource::Initialization
    );

    return newVector;
}

template JSValue vector_round<2> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_round<3> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_round<4> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

template <int components> JSValue vector_floor (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId = 0;
    const auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (this_val, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (container, components);

    JSValue newVector = container->adapter.instantiate ();
    const auto* newContainer = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (newVector, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (newContainer, components);

    newContainer->value.update (
	glm::floor (vector_get<components> (container->value)), DynamicValue::UpdateSource::Initialization
    );

    return newVector;
}

template JSValue vector_floor<2> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_floor<3> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_floor<4> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

template <int components> JSValue vector_ceil (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId = 0;
    const auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (this_val, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (container, components);

    JSValue newVector = container->adapter.instantiate ();
    const auto* newContainer = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (newVector, &classId));
    const auto vector = vector_get<components> (container->value);

    VEC_MAGIC_CHECK_EXCEPTION (newContainer, components);

    newContainer->value.update (glm::ceil (vector), DynamicValue::UpdateSource::Initialization);

    return newVector;
}

template JSValue vector_ceil<2> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_ceil<3> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_ceil<4> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

template <int components>
JSValue vector_toString (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId = 0;
    const auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (this_val, &classId));

    VEC_MAGIC_CHECK_EXCEPTION (container, components);

    return JS_NewString (ctx, container->value.toString ().c_str ());
}

template JSValue vector_toString<2> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_toString<3> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);
template JSValue vector_toString<4> (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv);

template <int components>
VectorAdapter<components>::VectorAdapter (ScriptEngine& engine) :
    ObjectAdapter (engine), m_instanceId (++VectorAdapterInstanceId), m_name ("Vec" + std::to_string (components)),
	m_exoticMethods (
	{
	    .get_own_property = vector_own_property<components>,
	    .get_own_property_names = vector_own_property_names<components>,
	    .get_property = vector_property_get<components>,
	    .set_property = vector_property_set<components>,
	}
    ) {
    vectorAdapterInstances<components>.emplace (this->m_instanceId, *this);
    this->registerType (
	{
	    .class_name = this->m_name.c_str (),
	    .finalizer = vector_finalizer<components>,
	    .exotic = &this->m_exoticMethods,
	}
    );

    // build the prototype for the Vector and assign the required methods
    m_prototype = JS_NewObject (this->m_engine.getContext ());

    JS_DupValue (this->m_engine.getContext (), m_prototype);

    JSValue ctor = JS_NewCFunctionMagic (
	this->m_engine.getContext (), vector_constructor<components>, this->m_name.c_str (), 1,
	JS_CFUNC_constructor_magic, this->m_instanceId
    );

    JS_SetConstructor (this->m_engine.getContext (), ctor, m_prototype);
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), m_prototype, "copy",
	JS_NewCFunction (this->m_engine.getContext (), vector_copy<components>, "copy", 0), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), m_prototype, "equals",
	JS_NewCFunction (this->m_engine.getContext (), vector_equals<components>, "equals", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), m_prototype, "length",
	JS_NewCFunction (this->m_engine.getContext (), vector_length<components>, "length", 0), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), m_prototype, "lengthSqr",
	JS_NewCFunction (this->m_engine.getContext (), vector_length<components>, "lengthSqr", 0), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), m_prototype, "normalize",
	JS_NewCFunction (this->m_engine.getContext (), vector_normalize<components>, "normalize", 0), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), m_prototype, "add",
	JS_NewCFunction (this->m_engine.getContext (), vector_add<components>, "add", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), m_prototype, "subtract",
	JS_NewCFunction (this->m_engine.getContext (), vector_subtract<components>, "subtract", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), m_prototype, "multiply",
	JS_NewCFunction (this->m_engine.getContext (), vector_multiply<components>, "multiply", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), m_prototype, "divide",
	JS_NewCFunction (this->m_engine.getContext (), vector_divide<components>, "divide", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), m_prototype, "dot",
	JS_NewCFunction (this->m_engine.getContext (), vector_dot<components>, "dot", 1), JS_PROP_ENUMERABLE
    );
    if constexpr (components == 3) {
	JS_DefinePropertyValueStr (
	    this->m_engine.getContext (), m_prototype, "cross",
	    JS_NewCFunction (this->m_engine.getContext (), vector_cross<components>, "cross", 1), JS_PROP_ENUMERABLE
	);
    }
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), m_prototype, "mix",
	JS_NewCFunction (this->m_engine.getContext (), vector_mix<components>, "mix", 2), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), m_prototype, "min",
	JS_NewCFunction (this->m_engine.getContext (), vector_min<components>, "min", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), m_prototype, "max",
	JS_NewCFunction (this->m_engine.getContext (), vector_max<components>, "max", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), m_prototype, "abs",
	JS_NewCFunction (this->m_engine.getContext (), vector_abs<components>, "abs", 0), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), m_prototype, "sign",
	JS_NewCFunction (this->m_engine.getContext (), vector_sign<components>, "sign", 0), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), m_prototype, "round",
	JS_NewCFunction (this->m_engine.getContext (), vector_round<components>, "round", 0), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), m_prototype, "floor",
	JS_NewCFunction (this->m_engine.getContext (), vector_floor<components>, "floor", 0), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), m_prototype, "ceil",
	JS_NewCFunction (this->m_engine.getContext (), vector_ceil<components>, "ceil", 0), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), m_prototype, "toString",
	JS_NewCFunction (this->m_engine.getContext (), vector_toString<components>, "toString", 0), JS_PROP_ENUMERABLE
    );

    JS_SetClassProto (this->m_engine.getContext (), this->m_classId, m_prototype);
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_engine.getGlobalThis (), m_name.c_str (),
	JS_DupValue (this->m_engine.getContext (), ctor), JS_PROP_ENUMERABLE
    );
    JS_FreeValue (this->m_engine.getContext (), ctor);
}

template <int components> VectorAdapter<components>::~VectorAdapter () {
    vectorAdapterInstances<components>.erase (this->m_instanceId);
    // ScriptEngine releases the JS handle before freeing the context, then
    // keeps this adapter alive until runtime finalizers have completed.
    if (!JS_IsUndefined (m_prototype)) this->releasePrototype ();
}

template <int components> void VectorAdapter<components>::releasePrototype () {
    if (JS_IsUndefined (m_prototype)) return;
    JS_FreeValue (this->m_engine.getContext (), m_prototype);
    m_prototype = JS_UNDEFINED;
}

template <int components> JSValue VectorAdapter<components>::instantiate (ScriptableObject& object) {
    throw new std::runtime_error ("Cannot create a Vector4 instance from a ScriptableObject");
}

template <int components> JSValue VectorAdapter<components>::instantiate (DynamicValue& value) {
    JSValue result = this->ObjectAdapter::instantiate (value);
    JS_SetOpaque (
	result,
	new VectorOpaqueContainer<components> {
	    .magic = VEC_OPAQUE_MAGIC + components,
	    .adapter = *this,
	    .value = value,
	    .id = InvalidVectorInstanceId,
	}
    );

    return result;
}

template <int components> JSValue VectorAdapter<components>::instantiate (DynamicValue& source, bool temporal) {
    auto value = std::make_unique<DynamicValue> (source);
    uint32_t id = ++VectorInstanceId;
    JSValue result = this->ObjectAdapter::instantiate (*value);
    JS_SetOpaque (
	result,
	new VectorOpaqueContainer<components> {
	    .magic = VEC_OPAQUE_MAGIC + components,
	    .adapter = *this,
	    .value = *value,
	    .id = id,
	}
    );

    this->m_values.emplace (id, std::move (value));

    return result;
}

template <int components> JSValue VectorAdapter<components>::instantiateLayerProperty (
    DynamicValue& source, std::function<bool ()> layerAlive
) {
    JSValue result = instantiate (source, true);
    if (JS_IsException (result)) return result;
    JSClassID classId = 0;
    auto* container = static_cast<VectorOpaqueContainer<components>*> (JS_GetAnyOpaque (result, &classId));
    if (!container || container->magic != static_cast<int> (VEC_OPAQUE_MAGIC + components)) {
        JS_FreeValue (this->m_engine.getContext (), result);
        return JS_ThrowInternalError (this->m_engine.getContext (), "Cannot bind layer vector snapshot");
    }
    container->layerProperty = &source;
    container->layerAlive = std::move (layerAlive);
    return result;
}

template <int components> JSValue VectorAdapter<components>::instantiate () {
    auto value = std::make_unique<DynamicValue> (vector_new<components> ());
    uint32_t id = ++VectorInstanceId;
    JSValue result = this->ObjectAdapter::instantiate (*value);
    JS_SetOpaque (
	result,
	new VectorOpaqueContainer<components> {
	    .magic = VEC_OPAQUE_MAGIC + components,
	    .adapter = *this,
	    .value = *value,
	    .id = id,
	}
    );

    this->m_values.emplace (id, std::move (value));

    return result;
}

template <int components> void VectorAdapter<components>::free (uint32_t vectorId) {
    auto it = this->m_values.find (vectorId);

    if (it != this->m_values.end ()) {
	this->m_values.erase (it);
    }
}

namespace WallpaperEngine::Scripting::Adapters {
template class VectorAdapter<2>;
template class VectorAdapter<3>;
template class VectorAdapter<4>;
} // namespace WallpaperEngine::Scripting::Adapters
