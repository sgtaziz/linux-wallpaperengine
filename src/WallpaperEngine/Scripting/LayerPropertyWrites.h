#pragma once

#include "quickjs.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace WallpaperEngine::Scripting {
enum class LayerWrite { Assign, Ignore, Exception };

/**
 * Native 2.8.42 native converts every host property write in one
 * function. It dispatches on the registered type through the jump
 * table and then calls the host setter. A value of the wrong
 * type jumps straight to the exit, so the property is left unchanged and no
 * exception is raised. The helpers below follow the cases one by one. Each
 * returns Ignore for a value that native skips.
 */

/**
 * String case 5, used by text, horizontalalign, verticalalign,
 * alignment, font and name:
 * - a JS string is used as it is;
 * - null or undefined (oddball kinds 3 and 4) are skipped, leaving the
 *   property unchanged without an exception;
 * any other value goes through v8::Value::ToString, so
 *   `text = Date.now()` stores the decimal string.
 * If the ToString call throws (for example, for a Symbol), the exception
 * reaches the script.
 */
inline LayerWrite convertLayerString (JSContext* context, JSValueConst value, std::string& result) {
    if (JS_IsNull (value) || JS_IsUndefined (value)) return LayerWrite::Ignore;
    // Convert explicitly: JS_ToCStringLen replaces a failed Error-object
    // conversion with its message, but native propagates the exception.
    JSValue string = JS_ToString (context, value);
    if (JS_IsException (string)) return LayerWrite::Exception;
    size_t length = 0;
    const char* chars = JS_ToCStringLen (context, &length, string);
    JS_FreeValue (context, string);
    if (!chars) return LayerWrite::Exception;
    result.assign (chars, length);
    JS_FreeCString (context, chars);
    return LayerWrite::Assign;
}

/**
 * Boolean cases 6 and 8. Both accept only a real
 * Boolean: is IsBoolean (an oddball map whose kind is false or
 * true). Numbers such as 0 or 1, strings and objects are skipped.
 */
inline LayerWrite convertLayerBoolean (JSValueConst value, bool& result) {
    if (!JS_IsBool (value)) return LayerWrite::Ignore;
    result = JS_VALUE_GET_BOOL (value) != 0;
    return LayerWrite::Assign;
}

/**
 * Int case 0. The value must pass IsNumber (: a Smi or a
 * HeapNumber). It is then converted by v8::Value::ToInt32 (, which
 * returns a Smi as it is and otherwise calls Object::ConvertToInt32
 * ), and read by v8::Int32::Value. This is ECMAScript
 * ToInt32: fractions truncate toward zero, values wrap modulo 2^32, and NaN or
 * Infinity become 0.
 */
inline LayerWrite convertLayerInt (JSContext* context, JSValueConst value, int32_t& result) {
    if (!JS_IsNumber (value)) return LayerWrite::Ignore;
    return JS_ToInt32 (context, &result, value) < 0 ? LayerWrite::Exception : LayerWrite::Assign;
}

/**
 * Narrows a double the way cvtsd2ss does under the default rounding mode:
 * NaN and Infinity are kept, and a finite value beyond the float range rounds
 * to the largest float or to Infinity. A plain cast would be undefined for an
 * out-of-range value.
 */
inline float narrowLayerFloat (double number) {
    constexpr double largest = std::numeric_limits<float>::max ();
    if (!std::isfinite (number) || std::abs (number) <= largest) return static_cast<float> (number);
    // The halfway point to 2^128 rounds to even, which is Infinity.
    const float magnitude = std::abs (number) >= largest + std::ldexp (1.0, 103)
        ? std::numeric_limits<float>::infinity () : std::numeric_limits<float>::max ();
    return std::signbit (number) ? -magnitude : magnitude;
}

/**
 * Float case 4. The value must pass IsNumber. Number::Value
 * is narrowed with cvtsd2ss, so NaN and Infinity are kept and
 * values beyond the float range become Infinity. No property registered on
 * layers sets the angle flag for this case.
 */
inline LayerWrite convertLayerFloat (JSContext* context, JSValueConst value, float& result) {
    if (!JS_IsNumber (value)) return LayerWrite::Ignore;
    double number = 0.0;
    if (JS_ToFloat64 (context, &number, value) < 0) return LayerWrite::Exception;
    result = narrowLayerFloat (number);
    return LayerWrite::Assign;
}

/** The float the native converter multiplies angle components by. */
inline constexpr float kLayerDegreesToRadians = 0.0174532924f;

/**
 * Vector cases 1, 2 and 3 (Vec4,
 * ). The converter does this:
 * For an object, it reads x, y, z and
 *   w in that order with Object::Get. Only after all reads does it require
 *   every component to pass IsNumber. If any component fails, the whole write
 *   is skipped. Missing components read as undefined, so a Vec2 written to a
 *   Vec3 property is skipped.
 * - Otherwise, a number is broadcast to every component.
 * - Anything else is skipped.
 * Components are narrowed to float without a finiteness check.
 *
 * The angle flag (registration bit 4, which only `angles` sets on
 * layers) multiplies object components by kLayerDegreesToRadians. The scalar
 * broadcast path skips that multiplication, so `angles = 90` stores 90
 * radians on every axis.
 *
 * A component read that throws leaves the exception pending. Native would
 * abort in ToLocalChecked at that point.
 */
inline LayerWrite convertLayerVector (JSContext* context, JSValueConst value, int count, bool angle,
                                      float* components) {
    if (count < 1 || count > 4) return LayerWrite::Ignore;
    if (JS_IsObject (value)) {
        JSValue read[4] {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};
        auto release = [&] {
            for (auto& component : read) JS_FreeValue (context, component);
        };
        for (int index = 0; index < count; ++index) {
            const char name[] = {"xyzw"[index], '\0'};
            read[index] = JS_GetPropertyStr (context, value, name);
            if (JS_IsException (read[index])) {
                read[index] = JS_UNDEFINED;
                release ();
                return LayerWrite::Exception;
            }
        }
        for (int index = 0; index < count; ++index) {
            if (!JS_IsNumber (read[index])) {
                release ();
                return LayerWrite::Ignore;
            }
        }
        for (int index = 0; index < count; ++index) {
            double number = 0.0;
            if (JS_ToFloat64 (context, &number, read[index]) < 0) {
                release ();
                return LayerWrite::Exception;
            }
            components[index] = narrowLayerFloat (number);
            if (angle) components[index] *= kLayerDegreesToRadians;
        }
        release ();
        return LayerWrite::Assign;
    }
    float scalar = 0.0f;
    const LayerWrite result = convertLayerFloat (context, value, scalar);
    if (result != LayerWrite::Assign) return result;
    for (int index = 0; index < count; ++index) components[index] = scalar;
    return LayerWrite::Assign;
}

/**
 * Maps a script property name to the key of the retained layer value.
 * Native host property names match exactly and case-sensitively: lookup
 * hashes the name with FNV-1a and
 * compares length and bytes, and each name is its own v8
 * accessor. The text settings are registered in lowercase (native
 * for example "backgroundbrightness"), but the Linux renderer keeps
 * them under the parser's camel-case keys. A lowercase public name maps to its
 * key. The camel-case key itself is not a native host name, so it returns
 * nullopt and the write becomes an expando.
 */
inline std::optional<std::string_view> layerPropertyKey (std::string_view name) {
    static constexpr std::pair<std::string_view, std::string_view> aliases[] {
        {"pointsize", "pointSize"},
        {"limitrows", "limitRows"},
        {"maxrows", "maxRows"},
        {"limitwidth", "limitWidth"},
        {"maxwidth", "maxWidth"},
        {"limituseellipsis", "limitUseEllipsis"},
        {"opaquebackground", "opaqueBackground"},
        {"backgroundcolor", "backgroundColor"},
        {"backgroundbrightness", "backgroundBrightness"},
    };
    for (const auto& [publicName, key] : aliases) {
        if (name == publicName) return key;
        if (name == key) return std::nullopt;
    }
    return name;
}

/**
 * Stores a write to a name the layer does not expose as an ordinary data
 * property of the receiver. Native layer wrappers come from a v8
 * ObjectTemplate. Each host property is an accessor and each host method is
 * a data member: 2.7.3, and 2.8.42 SetAccessor with
 * getter and setter. The template never installs a
 * named interceptor: the 2.8.42 ObjectTemplate::SetHandler has
 * no callers. Writing an unknown name, such as `opacity` on a text layer,
 * therefore creates an expando property that later reads return. The
 * handler is not aborted.
 */
inline int defineLayerExpando (JSContext* context, JSValueConst receiver, JSAtom name, JSValueConst value,
                               int flags) {
    return JS_DefinePropertyValue (context, receiver, name, JS_DupValue (context, value),
                                   JS_PROP_C_W_E | (flags & (JS_PROP_THROW | JS_PROP_THROW_STRICT)));
}
}
