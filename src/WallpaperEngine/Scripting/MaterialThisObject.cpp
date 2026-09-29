#include "MaterialThisObject.h"

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/UserSetting.h"
#include "WallpaperEngine/Scripting/Adapters/ScriptableObjectAdapter.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"

#include <cmath>
#include <cstdint>
#include <limits>

using namespace WallpaperEngine::Scripting;
using WallpaperEngine::Data::Model::DynamicValue;

namespace {
DynamicValue* resolve (JSContext* context, JSValue* data) {
    if (!Adapters::ScriptableObjectAdapter::resolve (data[0])) {
        JS_ThrowTypeError (context, "Material is no longer available");
        return nullptr;
    }
    int64_t raw = 0;
    if (JS_ToInt64 (context, &raw, data[1]) < 0) return nullptr;
    auto* value = reinterpret_cast<DynamicValue*> (static_cast<intptr_t> (raw));
    if (!value) JS_ThrowTypeError (context, "Material property is no longer available");
    return value;
}

JSValue valueGet (JSContext* context, JSValueConst, int, JSValueConst*, int, JSValue* data) {
    auto* value = resolve (context, data);
    if (!value) return JS_EXCEPTION;
    auto* engine = static_cast<ScriptEngine*> (JS_GetContextOpaque (context));
    // A script may retain the getter result after its image is destroyed.
    // The owner accessor is lifetime checked; return a value snapshot so a
    // retained Vec cannot outlive the material's DynamicValue storage.
    return engine->dynamicToJs (*value, true);
}

JSValue valueSet (JSContext* context, JSValueConst, int argc, JSValueConst* argv, int, JSValue* data) {
    auto* value = resolve (context, data);
    if (!value) return JS_EXCEPTION;
    if (argc != 1) return JS_ThrowTypeError (context, "Material property requires one value");
    const auto type = value->getType ();
    if (type == DynamicValue::Vec2 || type == DynamicValue::Vec3 || type == DynamicValue::Vec4) {
        if (!JS_IsObject (argv[0])) return JS_ThrowTypeError (context, "Material vector requires an object");
        float components[4] {};
        const int count = type == DynamicValue::Vec2 ? 2 : type == DynamicValue::Vec3 ? 3 : 4;
        for (int i = 0; i < count; ++i) {
            const char name[] {"xyzw"[i], '\0'};
            JSValue component = JS_GetPropertyStr (context, argv[0], name);
            if (JS_IsException (component)) return component;
            double number = 0.0;
            const bool valid = JS_IsNumber (component) && JS_ToFloat64 (context, &number, component) == 0 &&
                std::isfinite (number) && std::abs (number) <= std::numeric_limits<float>::max ();
            JS_FreeValue (context, component);
            if (!valid) return JS_ThrowTypeError (context, "Material vector requires finite components");
            components[i] = static_cast<float> (number);
        }
        if (count == 2) value->update (glm::vec2 (components[0], components[1]), DynamicValue::Script);
        else if (count == 3) value->update (glm::vec3 (components[0], components[1], components[2]), DynamicValue::Script);
        else value->update (glm::vec4 (components[0], components[1], components[2], components[3]), DynamicValue::Script);
        return JS_UNDEFINED;
    }
    if (type == DynamicValue::Float || type == DynamicValue::Int) {
        double number = 0.0;
        if (!JS_IsNumber (argv[0]) || JS_ToFloat64 (context, &number, argv[0]) < 0 ||
            !std::isfinite (number) || std::abs (number) > std::numeric_limits<float>::max ())
            return JS_ThrowTypeError (context, "Material scalar requires a finite number");
        if (type == DynamicValue::Int) {
            if (std::trunc (number) != number || number < std::numeric_limits<int32_t>::min () ||
                number > std::numeric_limits<int32_t>::max ())
                return JS_ThrowTypeError (context, "Material integer is out of range");
            value->update (static_cast<int32_t> (number), DynamicValue::Script);
        } else value->update (static_cast<float> (number), DynamicValue::Script);
        return JS_UNDEFINED;
    }
    if (type == DynamicValue::Boolean && JS_IsBool (argv[0])) {
        value->update (JS_ToBool (context, argv[0]) != 0, DynamicValue::Script);
        return JS_UNDEFINED;
    }
    return JS_ThrowTypeError (context, "Unsupported material property assignment");
}
}

JSValue WallpaperEngine::Scripting::createMaterialThisObject (
    JSContext* context, JSValueConst layer, const Data::Model::ShaderConstantMap& constants) {
    JSValue object = JS_NewObject (context);
    if (JS_IsException (object)) return object;
    for (const auto& [name, setting] : constants) {
        if (!setting || !setting->value) continue;
        JSValue captured[] {JS_DupValue (context, layer),
            JS_NewInt64 (context, static_cast<int64_t> (reinterpret_cast<intptr_t> (setting->value.get ())))};
        JSValue getter = JS_NewCFunctionData (context, valueGet, 0, 0, 2, captured);
        JSValue setter = JS_NewCFunctionData (context, valueSet, 1, 0, 2, captured);
        JS_FreeValue (context, captured[0]);
        JS_FreeValue (context, captured[1]);
        JSAtom atom = JS_NewAtom (context, name.c_str ());
        const int defined = JS_DefinePropertyGetSet (context, object, atom, getter, setter,
                                                     JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
        JS_FreeAtom (context, atom);
        if (defined < 0) { JS_FreeValue (context, object); return JS_EXCEPTION; }
    }
    return object;
}
